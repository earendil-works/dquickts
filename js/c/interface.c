/*
 * dquickts WebAssembly interface
 *
 * A small C layer over quickjs.h that the JavaScript bindings in
 * js/src/ call. It is linked with the engine into a WASI reactor module
 * (quickjs.wasm): no main(), the host calls the qjs_* exports.
 *
 * One instance holds one runtime with one context. Values cross the
 * boundary as pointers to JSValue boxes allocated with malloc(): the host
 * owns every box it receives and releases it with qjs_free_value(), which
 * frees the value and the box. Strings are passed as UTF-8 in buffers the
 * host allocates with qjs_malloc(); string results are read from the
 * buffer returned by qjs_to_string() and released with qjs_free_cstring().
 *
 * Host imports (module "env"):
 *   host_call(func_id, this_ptr, argc, argv_ptr) -> box
 *       a function made with qjs_new_function() was called. 'this_ptr'
 *       and 'argv_ptr' point to borrowed JSValues valid during the call.
 *       Return a box (ownership passes to the engine), or 0 after
 *       qjs_throw() to throw.
 *   host_interrupt() -> int
 *       polled while JS runs when enabled with qjs_set_interrupt_handler();
 *       non-zero interrupts the execution with an uncatchable error.
 *   host_timezone_offset(seconds) -> int
 *       offset of local time from UTC in seconds (positive east of UTC)
 *       at 'seconds' since the epoch. WASI has no time zones, so libc asks
 *       the host through the __secs_to_zone() wrapper below.
 */

#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"

#define EXPORT(name) __attribute__((export_name(#name)))
#define IMPORT(name) __attribute__((import_module("env"), import_name(#name)))

IMPORT(host_call)
JSValue *host_call(int func_id, JSValueConst *this_ptr, int argc, JSValueConst *argv);
IMPORT(host_interrupt)
int host_interrupt(void);
IMPORT(host_timezone_offset)
int host_timezone_offset(double seconds);

static JSRuntime *rt;
static JSContext *ctx;
/* length of the last string returned by qjs_to_string() */
static size_t last_len;

/* Intrinsic flags given to qjs_init(); must match Intrinsics in index.ts */
#define QJS_INTRINSIC_DATE             (1 << 0)
#define QJS_INTRINSIC_EVAL             (1 << 1)
#define QJS_INTRINSIC_REGEXP           (1 << 2)
#define QJS_INTRINSIC_JSON             (1 << 3)
#define QJS_INTRINSIC_PROXY            (1 << 4)
#define QJS_INTRINSIC_MAP_SET          (1 << 5)
#define QJS_INTRINSIC_TYPED_ARRAYS     (1 << 6)
#define QJS_INTRINSIC_PROMISE          (1 << 7)
#define QJS_INTRINSIC_BIG_INT          (1 << 8) /* always present */
#define QJS_INTRINSIC_WEAK_REF         (1 << 9)
#define QJS_INTRINSIC_PERFORMANCE      (1 << 10)
#define QJS_INTRINSIC_DOM_EXCEPTION    (1 << 11)
#define QJS_INTRINSIC_ATOB_BTOA        (1 << 12)
#define QJS_INTRINSIC_STRING_NORMALIZE (1 << 13)
#define QJS_INTRINSIC_TEMPORAL         (1 << 14)
#define QJS_INTRINSIC_INTL             (1 << 15)

/* Default for JS_SetMaxStackSize(); the wasm stack is 1 MiB (see the
   Makefile), which leaves room for the C frames below the JS ones. */
#define QJS_DEFAULT_MAX_STACK_SIZE (512 * 1024)

/* ---- Memory accounting and limit ----

   The default allocator of the engine counts malloc_usable_size(), which
   is not available on WASI, so it would only count the per allocation
   overhead and a memory limit would never trigger. wasi-libc's dlmalloc
   has malloc_usable_size(), so these functions do the accounting.

   The limit is enforced here and not with the engine's malloc_limit: once
   a script has used all the memory, the engine needs a little more to
   create the "out of memory" InternalError. Allocations are refused at a
   soft limit QJS_OOM_HEADROOM below the limit; after a refusal they may
   use the headroom until the usage drops below the soft limit again. The
   limit itself is never exceeded. */

#define QJS_OOM_HEADROOM (64 * 1024)

static size_t mem_limit; /* 0: unlimited */
static int mem_in_oom;

static size_t mem_soft_limit(void)
{
    if (mem_limit > 2 * QJS_OOM_HEADROOM)
        return mem_limit - QJS_OOM_HEADROOM;
    return mem_limit / 2;
}

static int mem_exceeds(JSMallocState *s, size_t size, size_t cap)
{
    return size > cap || s->malloc_size > cap - size;
}

/* non-zero if an allocation of 'size' more bytes must be refused */
static int mem_refuse(JSMallocState *s, size_t size)
{
    if (mem_limit == 0)
        return 0;
    if (!mem_exceeds(s, size, mem_soft_limit())) {
        mem_in_oom = 0;
        return 0;
    }
    if (mem_in_oom && !mem_exceeds(s, size, mem_limit))
        return 0;
    mem_in_oom = 1;
    return 1;
}

static void *wasm_calloc(JSMallocState *s, size_t count, size_t size)
{
    void *ptr;
    if (size != 0 && count > SIZE_MAX / size)
        return NULL;
    if (mem_refuse(s, count * size))
        return NULL;
    ptr = calloc(count, size);
    if (ptr) {
        s->malloc_count++;
        s->malloc_size += malloc_usable_size(ptr);
    }
    return ptr;
}

static void *wasm_malloc(JSMallocState *s, size_t size)
{
    void *ptr;
    if (mem_refuse(s, size))
        return NULL;
    ptr = malloc(size);
    if (ptr) {
        s->malloc_count++;
        s->malloc_size += malloc_usable_size(ptr);
    }
    return ptr;
}

static void wasm_free(JSMallocState *s, void *ptr)
{
    if (!ptr)
        return;
    s->malloc_count--;
    s->malloc_size -= malloc_usable_size(ptr);
    free(ptr);
}

static void *wasm_realloc(JSMallocState *s, void *ptr, size_t size)
{
    size_t old_size;
    void *new_ptr;

    if (!ptr)
        return size ? wasm_malloc(s, size) : NULL;
    old_size = malloc_usable_size(ptr);
    if (size == 0) {
        wasm_free(s, ptr);
        return NULL;
    }
    /* never refuse to shrink */
    if (size > old_size && mem_refuse(s, size - old_size))
        return NULL;
    new_ptr = realloc(ptr, size);
    if (!new_ptr)
        return NULL;
    s->malloc_size = s->malloc_size - old_size + malloc_usable_size(new_ptr);
    return new_ptr;
}

static size_t wasm_malloc_usable_size(const void *ptr)
{
    return malloc_usable_size((void *)ptr);
}

static const JSMallocFunctions wasm_malloc_funcs = {
    wasm_calloc,
    wasm_malloc,
    wasm_free,
    wasm_realloc,
    wasm_malloc_usable_size,
};

/* ---- Time zone ----

   Linked with -Wl,--wrap=__secs_to_zone: the libc time zone lookup used
   by localtime_r() (Date's local time) asks the host. */

extern const char __utc[];

void __wrap___secs_to_zone(long long t, int local, int *isdst, long *offset,
                           long *oppoff, const char **zonename)
{
    int off = host_timezone_offset((double)t);
    (void)local;
    if (isdst)
        *isdst = 0;
    if (offset)
        *offset = off;
    if (oppoff)
        *oppoff = off;
    if (zonename)
        *zonename = off == 0 ? __utc : "LMT";
}

/* ---- Helpers ---- */

/* box a value for the host; the value is freed if out of memory */
static JSValue *box(JSValue v)
{
    JSValue *p = malloc(sizeof(JSValue));
    if (!p) {
        JS_FreeValue(ctx, v);
        return NULL;
    }
    *p = v;
    return p;
}

static int interrupt_trampoline(JSRuntime *rt, void *opaque)
{
    (void)rt;
    (void)opaque;
    return host_interrupt();
}

/* func_data[0] is the host function id */
static JSValue host_function_trampoline(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv,
                                        int magic, JSValueConst *func_data)
{
    JSValue *res;
    JSValue val;
    int func_id;

    (void)magic;
    if (JS_ToInt32(ctx, &func_id, func_data[0]))
        return JS_EXCEPTION;
    res = host_call(func_id, &this_val, argc, argv);
    if (!res)
        return JS_EXCEPTION;
    val = *res;
    free(res);
    return val;
}

/* ---- Lifecycle ---- */

EXPORT(qjs_version)
const char *qjs_version(void)
{
    return JS_GetVersion();
}

EXPORT(qjs_sizeof_value)
int qjs_sizeof_value(void)
{
    return sizeof(JSValue);
}

EXPORT(qjs_has_temporal_intl)
int qjs_has_temporal_intl(void)
{
#ifdef CONFIG_TEMPORAL_INTL
    return 1;
#else
    return 0;
#endif
}

static int add_intrinsics(JSContext *ctx, unsigned int flags)
{
    if (JS_AddIntrinsicBaseObjects(ctx))
        return -1;
    if ((flags & QJS_INTRINSIC_DATE) && JS_AddIntrinsicDate(ctx))
        return -1;
    if ((flags & QJS_INTRINSIC_EVAL) && JS_AddIntrinsicEval(ctx))
        return -1;
    if ((flags & QJS_INTRINSIC_STRING_NORMALIZE) &&
        JS_AddIntrinsicStringNormalize(ctx))
        return -1;
    if ((flags & QJS_INTRINSIC_REGEXP) && JS_AddIntrinsicRegExp(ctx))
        return -1;
    if ((flags & QJS_INTRINSIC_JSON) && JS_AddIntrinsicJSON(ctx))
        return -1;
    if ((flags & QJS_INTRINSIC_PROXY) && JS_AddIntrinsicProxy(ctx))
        return -1;
    if ((flags & QJS_INTRINSIC_MAP_SET) && JS_AddIntrinsicMapSet(ctx))
        return -1;
    if ((flags & QJS_INTRINSIC_TYPED_ARRAYS) && JS_AddIntrinsicTypedArrays(ctx))
        return -1;
    if ((flags & QJS_INTRINSIC_PROMISE) && JS_AddIntrinsicPromise(ctx))
        return -1;
    if ((flags & QJS_INTRINSIC_WEAK_REF) && JS_AddIntrinsicWeakRef(ctx))
        return -1;
    if ((flags & QJS_INTRINSIC_DOM_EXCEPTION) && JS_AddIntrinsicDOMException(ctx))
        return -1;
    /* also adds DOMException */
    if ((flags & QJS_INTRINSIC_ATOB_BTOA) && JS_AddIntrinsicAToB(ctx))
        return -1;
    if ((flags & QJS_INTRINSIC_PERFORMANCE) && JS_AddPerformance(ctx))
        return -1;
#ifdef CONFIG_TEMPORAL_INTL
    if ((flags & QJS_INTRINSIC_TEMPORAL) && JS_AddIntrinsicTemporal(ctx))
        return -1;
    if ((flags & QJS_INTRINSIC_INTL) && JS_AddIntrinsicIntl(ctx))
        return -1;
#endif
    return 0;
}

/* Create the runtime and its context. The memory limit applies while the
   context is created. Return 0 on success. */
EXPORT(qjs_init)
int qjs_init(unsigned int intrinsics, size_t memory_limit, size_t max_stack_size)
{
    if (rt)
        return -1;
    mem_limit = memory_limit;
    mem_in_oom = 0;
    rt = JS_NewRuntime2(&wasm_malloc_funcs, NULL);
    if (!rt)
        return -1;
    JS_SetMaxStackSize(rt, max_stack_size ? max_stack_size
                                          : QJS_DEFAULT_MAX_STACK_SIZE);
    ctx = JS_NewContextRaw(rt);
    if (!ctx || add_intrinsics(ctx, intrinsics)) {
        if (ctx)
            JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        ctx = NULL;
        rt = NULL;
        return -1;
    }
    return 0;
}

EXPORT(qjs_destroy)
void qjs_destroy(void)
{
    if (ctx) {
        JS_FreeContext(ctx);
        ctx = NULL;
    }
    if (rt) {
        JS_FreeRuntime(rt);
        rt = NULL;
    }
}

EXPORT(qjs_set_memory_limit)
void qjs_set_memory_limit(size_t limit)
{
    mem_limit = limit;
    mem_in_oom = 0;
}

EXPORT(qjs_set_max_stack_size)
void qjs_set_max_stack_size(size_t size)
{
    JS_SetMaxStackSize(rt, size);
}

EXPORT(qjs_set_interrupt_handler)
void qjs_set_interrupt_handler(int enable)
{
    JS_SetInterruptHandler(rt, enable ? interrupt_trampoline : NULL, NULL);
}

EXPORT(qjs_set_gc_threshold)
void qjs_set_gc_threshold(size_t threshold)
{
    JS_SetGCThreshold(rt, threshold);
}

EXPORT(qjs_run_gc)
void qjs_run_gc(void)
{
    JS_RunGC(rt);
}

/* bytes allocated by the engine */
EXPORT(qjs_memory_used)
double qjs_memory_used(void)
{
    JSMemoryUsage s;
    JS_ComputeMemoryUsage(rt, &s);
    return (double)s.malloc_size;
}

/* ---- Buffers ---- */

EXPORT(qjs_malloc)
void *qjs_malloc(size_t size)
{
    return malloc(size ? size : 1);
}

EXPORT(qjs_free)
void qjs_free(void *ptr)
{
    free(ptr);
}

/* ---- Evaluation and calls ---- */

/* 'code' must be followed by a zero byte */
EXPORT(qjs_eval)
JSValue *qjs_eval(const char *code, size_t len, const char *filename, int flags)
{
    return box(JS_Eval(ctx, code, len, filename, flags));
}

EXPORT(qjs_call)
JSValue *qjs_call(JSValue *func, JSValue *this_val, int argc, JSValue **argv)
{
    JSValueConst args[argc > 0 ? argc : 1];
    for (int i = 0; i < argc; i++)
        args[i] = *argv[i];
    return box(JS_Call(ctx, *func, *this_val, argc, args));
}

EXPORT(qjs_call_constructor)
JSValue *qjs_call_constructor(JSValue *func, int argc, JSValue **argv)
{
    JSValueConst args[argc > 0 ? argc : 1];
    for (int i = 0; i < argc; i++)
        args[i] = *argv[i];
    return box(JS_CallConstructor(ctx, *func, argc, args));
}

/* a function that calls host_call(func_id, ...) */
EXPORT(qjs_new_function)
JSValue *qjs_new_function(int func_id, const char *name, int length)
{
    JSValue data = JS_NewInt32(ctx, func_id);
    return box(JS_NewCFunctionData2(ctx, host_function_trampoline, name,
                                    length, 0, 1, &data));
}

/* ---- Jobs ---- */

EXPORT(qjs_is_job_pending)
int qjs_is_job_pending(void)
{
    return JS_IsJobPending(rt);
}

/* 1 if a job ran, 0 if none was pending, -1 if it threw (the exception
   is pending: see qjs_get_exception()) */
EXPORT(qjs_execute_pending_job)
int qjs_execute_pending_job(void)
{
    JSContext *job_ctx;
    return JS_ExecutePendingJob(rt, &job_ctx);
}

/* ---- Exceptions ---- */

EXPORT(qjs_get_exception)
JSValue *qjs_get_exception(void)
{
    return box(JS_GetException(ctx));
}

EXPORT(qjs_is_uncatchable_exception)
int qjs_is_uncatchable_exception(void)
{
    return JS_IsUncatchableException(ctx);
}

/* throw a copy of 'val'; for host_call(), which then returns 0. An
   uncatchable exception (e.g. an interruption) propagating through a host
   function must stay uncatchable. */
EXPORT(qjs_throw)
void qjs_throw(JSValue *val, int uncatchable)
{
    JS_Throw(ctx, JS_DupValue(ctx, *val));
    if (uncatchable)
        JS_SetUncatchableException(ctx, 1);
}

EXPORT(qjs_new_error)
JSValue *qjs_new_error(void)
{
    return box(JS_NewError(ctx));
}

/* ---- Values ---- */

EXPORT(qjs_dup_value)
JSValue *qjs_dup_value(JSValue *val)
{
    return box(JS_DupValue(ctx, *val));
}

EXPORT(qjs_free_value)
void qjs_free_value(JSValue *val)
{
    if (!val)
        return;
    JS_FreeValue(ctx, *val);
    free(val);
}

EXPORT(qjs_get_undefined)
JSValue *qjs_get_undefined(void)
{
    return box(JS_UNDEFINED);
}

EXPORT(qjs_get_null)
JSValue *qjs_get_null(void)
{
    return box(JS_NULL);
}

EXPORT(qjs_new_bool)
JSValue *qjs_new_bool(int b)
{
    return box(JS_NewBool(ctx, b));
}

EXPORT(qjs_new_number)
JSValue *qjs_new_number(double d)
{
    return box(JS_NewFloat64(ctx, d));
}

/* 'str' must be followed by a zero byte */
EXPORT(qjs_new_string)
JSValue *qjs_new_string(const char *str, size_t len)
{
    return box(JS_NewStringLen(ctx, str, len));
}

EXPORT(qjs_new_object)
JSValue *qjs_new_object(void)
{
    return box(JS_NewObject(ctx));
}

EXPORT(qjs_new_array)
JSValue *qjs_new_array(void)
{
    return box(JS_NewArray(ctx));
}

EXPORT(qjs_get_global)
JSValue *qjs_get_global(void)
{
    return box(JS_GetGlobalObject(ctx));
}

/* 'json' must be followed by a zero byte */
EXPORT(qjs_parse_json)
JSValue *qjs_parse_json(const char *json, size_t len, const char *filename)
{
    return box(JS_ParseJSON(ctx, json, len, filename));
}

EXPORT(qjs_json_stringify)
JSValue *qjs_json_stringify(JSValue *val)
{
    return box(JS_JSONStringify(ctx, *val, JS_UNDEFINED, JS_UNDEFINED));
}

/* Values of typeof; must match TYPEOF in index.ts */
enum {
    QJS_TYPE_UNDEFINED,
    QJS_TYPE_OBJECT, /* includes null */
    QJS_TYPE_BOOLEAN,
    QJS_TYPE_NUMBER,
    QJS_TYPE_STRING,
    QJS_TYPE_SYMBOL,
    QJS_TYPE_BIGINT,
    QJS_TYPE_FUNCTION,
    QJS_TYPE_EXCEPTION,
};

EXPORT(qjs_typeof)
int qjs_typeof(JSValue *val)
{
    JSValueConst v = *val;
    if (JS_IsUndefined(v) || JS_IsUninitialized(v))
        return QJS_TYPE_UNDEFINED;
    if (JS_IsException(v))
        return QJS_TYPE_EXCEPTION;
    if (JS_IsBool(v))
        return QJS_TYPE_BOOLEAN;
    if (JS_IsNumber(v))
        return QJS_TYPE_NUMBER;
    if (JS_IsString(v))
        return QJS_TYPE_STRING;
    if (JS_IsSymbol(v))
        return QJS_TYPE_SYMBOL;
    if (JS_IsBigInt(ctx, v))
        return QJS_TYPE_BIGINT;
    if (JS_IsFunction(ctx, v))
        return QJS_TYPE_FUNCTION;
    return QJS_TYPE_OBJECT;
}

EXPORT(qjs_is_null)
int qjs_is_null(JSValue *val)
{
    return JS_IsNull(*val);
}

EXPORT(qjs_is_array)
int qjs_is_array(JSValue *val)
{
    return JS_IsArray(ctx, *val) > 0;
}

EXPORT(qjs_is_error)
int qjs_is_error(JSValue *val)
{
    return JS_IsError(ctx, *val);
}

EXPORT(qjs_is_promise)
int qjs_is_promise(JSValue *val)
{
    return JS_IsPromise(*val);
}

/* -1 on exception */
EXPORT(qjs_to_bool)
int qjs_to_bool(JSValue *val)
{
    return JS_ToBool(ctx, *val);
}

/* NaN on exception (check qjs_has_exception()) */
EXPORT(qjs_to_number)
double qjs_to_number(JSValue *val)
{
    double d;
    if (JS_ToFloat64(ctx, &d, *val))
        return __builtin_nan("");
    return d;
}

EXPORT(qjs_has_exception)
int qjs_has_exception(void)
{
    return JS_HasException(ctx);
}

/* UTF-8 conversion of 'val' with String(), length in qjs_last_length();
   NULL on exception. Free with qjs_free_cstring(). */
EXPORT(qjs_to_string)
const char *qjs_to_string(JSValue *val)
{
    return JS_ToCStringLen(ctx, &last_len, *val);
}

EXPORT(qjs_last_length)
size_t qjs_last_length(void)
{
    return last_len;
}

EXPORT(qjs_free_cstring)
void qjs_free_cstring(const char *str)
{
    JS_FreeCString(ctx, str);
}

/* ---- Properties ---- */

EXPORT(qjs_get_prop)
JSValue *qjs_get_prop(JSValue *obj, const char *name)
{
    return box(JS_GetPropertyStr(ctx, *obj, name));
}

/* -1 on exception */
EXPORT(qjs_set_prop)
int qjs_set_prop(JSValue *obj, const char *name, JSValue *val)
{
    return JS_SetPropertyStr(ctx, *obj, name, JS_DupValue(ctx, *val));
}

EXPORT(qjs_get_prop_value)
JSValue *qjs_get_prop_value(JSValue *obj, JSValue *key)
{
    JSAtom atom = JS_ValueToAtom(ctx, *key);
    JSValue res;
    if (atom == JS_ATOM_NULL)
        return box(JS_EXCEPTION);
    res = JS_GetProperty(ctx, *obj, atom);
    JS_FreeAtom(ctx, atom);
    return box(res);
}

EXPORT(qjs_set_prop_value)
int qjs_set_prop_value(JSValue *obj, JSValue *key, JSValue *val)
{
    JSAtom atom = JS_ValueToAtom(ctx, *key);
    int res;
    if (atom == JS_ATOM_NULL)
        return -1;
    res = JS_SetProperty(ctx, *obj, atom, JS_DupValue(ctx, *val));
    JS_FreeAtom(ctx, atom);
    return res;
}

EXPORT(qjs_get_index)
JSValue *qjs_get_index(JSValue *obj, uint32_t idx)
{
    return box(JS_GetPropertyUint32(ctx, *obj, idx));
}

EXPORT(qjs_set_index)
int qjs_set_index(JSValue *obj, uint32_t idx, JSValue *val)
{
    return JS_SetPropertyUint32(ctx, *obj, idx, JS_DupValue(ctx, *val));
}

/* ---- Promises ---- */

/* the promise, its resolve and reject functions in out[0..1] */
EXPORT(qjs_new_promise)
JSValue *qjs_new_promise(JSValue **out)
{
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        out[0] = out[1] = NULL;
        return box(promise);
    }
    out[0] = box(funcs[0]);
    out[1] = box(funcs[1]);
    return box(promise);
}

/* 0 pending, 1 fulfilled, 2 rejected, -1 not a promise */
EXPORT(qjs_promise_state)
int qjs_promise_state(JSValue *promise)
{
    return JS_PromiseState(ctx, *promise);
}

EXPORT(qjs_promise_result)
JSValue *qjs_promise_result(JSValue *promise)
{
    return box(JS_PromiseResult(ctx, *promise));
}
