/*
 * Heap snapshot API tests: native registry completeness, host classes,
 * host roots, several contexts (realms), incremental snapshots, native
 * call suspension and error handling.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"

static int failures;

#define CHECK(cond) do {                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* host class with native state */
typedef struct {
    int count;
} Counter;

static JSClassID counter_class_id;

static void counter_finalizer(JSRuntime *rt, JSValue val)
{
    js_free_rt(rt, JS_GetOpaque(val, counter_class_id));
}

static JSClassDef counter_class = {
    "Counter",
    .finalizer = counter_finalizer,
};

static JSValue js_counter_new(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    JSValue obj = JS_NewObjectClass(ctx, counter_class_id);
    Counter *c = js_mallocz(ctx, sizeof(*c));
    JS_SetOpaque(obj, c);
    return obj;
}

static JSValue js_counter_inc(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    Counter *c = JS_GetOpaque2(ctx, this_val, counter_class_id);
    if (!c)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, ++c->count);
}

static JSValue save_opaque(JSContext *ctx, JSValueConst obj, void *opaque)
{
    Counter *c = JS_GetOpaque(obj, counter_class_id);
    CHECK(opaque == &counter_class);
    /* host hooks may allocate: collect garbage in the middle of the write */
    JS_RunGC(JS_GetRuntime(ctx));
    return JS_NewInt32(ctx, c->count);
}

static int load_opaque(JSContext *ctx, JSValueConst obj, JSValueConst saved, void *opaque)
{
    Counter *c = js_mallocz(ctx, sizeof(*c));
    if (!c || JS_ToInt32(ctx, &c->count, saved))
        return -1;
    JS_SetOpaque(obj, c);
    return 0;
}

/* snapshot attempted from JS code: must fail */
static JSValue js_try_snapshot(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    size_t size;
    uint8_t *buf = JS_WriteSnapshot(ctx, &size, NULL, NULL, 0, 0);
    if (buf) {
        js_free(ctx, buf);
        return JS_FALSE;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    return JS_TRUE;
}

/* suspend the calling JS code until the host gives the result */
static JSValue js_pause(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    return JS_SuspendCall(ctx);
}

/* iterator 'next' method called directly by the engine (no call
   frame): a native continuation */
static JSValue js_pause_next(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int *pdone, int magic)
{
    return JS_SuspendCall(ctx);
}

/* host class whose exotic property reads run JS code */
static JSClassID probe_class_id;

static JSValue probe_get_property(JSContext *ctx, JSValueConst obj, JSAtom atom,
                                  JSValueConst receiver)
{
    return JS_Eval(ctx, "pause()", 7, "<probe>", JS_EVAL_TYPE_GLOBAL);
}

static JSClassExoticMethods probe_exotic = { .get_property = probe_get_property };
static JSClassDef probe_class = { "Probe", .exotic = &probe_exotic };

static const JSCFunctionListEntry counter_funcs[] = {
    JS_CFUNC_DEF("newCounter", 0, js_counter_new),
    JS_CFUNC_DEF("inc", 0, js_counter_inc),
    JS_CFUNC_DEF("pause", 1, js_pause),
    JS_ITERATOR_NEXT_DEF("pauseNext", 0, js_pause_next, 0),
};
static const JSCFunctionListEntry *host_lists[] = { counter_funcs };
#define COUNTER_FUNCS_LEN (sizeof(counter_funcs) / sizeof(counter_funcs[0]))
static const int host_list_lens[] = { COUNTER_FUNCS_LEN };

static const JSSnapshotHost host = {
    .func_lists = host_lists,
    .func_list_lens = host_list_lens,
    .func_list_count = 1,
    .save_opaque = save_opaque,
    .load_opaque = load_opaque,
    .opaque = &counter_class,
};

static JSRuntime *new_runtime(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JS_NewClass(rt, counter_class_id, &counter_class);
    return rt;
}

static JSContext *new_context(JSRuntime *rt)
{
    JSContext *ctx = JS_NewContext(rt);
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyFunctionList(ctx, global, counter_funcs, COUNTER_FUNCS_LEN);
    JS_FreeValue(ctx, global);
    return ctx;
}

static JSValue eval(JSContext *ctx, const char *src)
{
    JSValue val = JS_Eval(ctx, src, strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(val)) {
        JSValue exc = JS_GetException(ctx);
        const char *str = JS_ToCString(ctx, exc);
        fprintf(stderr, "exception: %s\n", str);
        JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, exc);
        failures++;
    }
    return val;
}

static int eval_int(JSContext *ctx, const char *src)
{
    JSValue val = eval(ctx, src);
    int32_t v = -1;
    JS_ToInt32(ctx, &v, val);
    JS_FreeValue(ctx, val);
    return v;
}

/* the snapshot buffer is owned by the runtime: return a malloc copy */
static uint8_t *write_snapshot_flags(JSContext *ctx, size_t *psize, JSValueConst *roots,
                                     int n, int flags)
{
    uint8_t *snap = JS_WriteSnapshot(ctx, psize, &host, roots, n, flags), *buf;
    if (!snap) {
        eval(ctx, "throw 0"); /* not reached: report the pending exception */
        return NULL;
    }
    buf = malloc(*psize);
    memcpy(buf, snap, *psize);
    js_free(ctx, snap);
    return buf;
}

static uint8_t *write_snapshot(JSContext *ctx, size_t *psize, JSValueConst *roots, int n)
{
    return write_snapshot_flags(ctx, psize, roots, n, 0);
}

/* read 'buf' (with the delta 'delta' if not NULL) in a new runtime */
static JSContext *read_snapshot(const uint8_t *buf, size_t size,
                                const uint8_t *delta, size_t delta_size)
{
    JSRuntime *rt = new_runtime();
    JSSnapshotResult res;
    JSContext *ctx;
    char err[256];

    if (JS_ReadSnapshot(rt, buf, size, delta, delta_size, &host, &res, err, sizeof(err))) {
        fprintf(stderr, "read: %s\n", err);
        failures++;
        JS_FreeRuntime(rt);
        return NULL;
    }
    ctx = res.contexts[0];
    JS_FreeSnapshotResult(rt, &res);
    return ctx;
}

static void free_context_and_runtime(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* the string conversion of 'val' (freed) is 'expected' */
static void check_string(JSContext *ctx, JSValue val, const char *expected, int line)
{
    const char *str = JS_IsException(val) ? NULL : JS_ToCString(ctx, val);
    if (!str || strcmp(str, expected)) {
        fprintf(stderr, "test_snapshot_api.c:%d: got '%s', expected '%s'\n",
                line, str ? str : "<exception>", expected);
        failures++;
    }
    JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, val);
}

#define CHECK_STRING(ctx, val, expected) check_string(ctx, val, expected, __LINE__)

/* JS_SuspendCall(): a C function suspends its JS caller at the call,
   which completes with the value given to JS_ResumeWith(), possibly in
   another runtime. */
static void test_suspend_call(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_context(rt), *ctx2;
    JSValue val;
    uint8_t *snap;
    size_t size;
    int i;

    JS_FreeValue(ctx, eval(ctx,
        "var log = [];"
        "function work(a) { log.push('before ' + a); const r = pause(a);"
        "  log.push('after ' + r); return r * 2; }"
        "function sum() { let s = 0; for (let i = 0; i < 3; i++) s += pause(i); return s; }"
        "function caught() { try { return pause(); } catch (e) { return 'caught ' + e; } }"
        "const bound = pause.bind(null);"
        "function viaBound() { return bound() + 1; }"
        "function tail() { return pause(); }"));

    /* suspended at the call, then saved and completed elsewhere */
    val = JS_Eval(ctx, "work(20)", 8, "<test>", JS_EVAL_TYPE_GLOBAL);
    CHECK(JS_IsException(val) && JS_IsSuspended(rt) && JS_IsSuspendException(ctx));
    snap = write_snapshot(ctx, &size, NULL, 0);
    CHECK(snap != NULL);
    for(i = 0; i < 2 && snap; i++) {
        ctx2 = read_snapshot(snap, size, NULL, 0);
        if (!ctx2)
            break;
        CHECK(JS_IsSuspended(JS_GetRuntime(ctx2)));
        if (i == 0) {
            CHECK_STRING(ctx2, JS_ResumeWith(ctx2, JS_NewInt32(ctx2, 21), 0), "42");
            CHECK_STRING(ctx2, eval(ctx2, "log.join()"), "before 20,after 21");
        } else {
            /* JS_Resume(): the call returns undefined */
            CHECK_STRING(ctx2, JS_Resume(ctx2), "NaN");
            CHECK_STRING(ctx2, eval(ctx2, "log.join()"), "before 20,after undefined");
        }
        free_context_and_runtime(ctx2);
    }
    free(snap);
    CHECK_STRING(ctx, JS_ResumeWith(ctx, JS_NewInt32(ctx, 1), 0), "2");

    /* several suspensions of a resumed frame */
    val = JS_Eval(ctx, "sum()", 5, "<test>", JS_EVAL_TYPE_GLOBAL);
    for(i = 1; i <= 2; i++) {
        CHECK(JS_IsException(val) && JS_IsSuspended(rt));
        val = JS_ResumeWith(ctx, JS_NewInt32(ctx, i * 10), 0);
    }
    CHECK_STRING(ctx, JS_ResumeWith(ctx, JS_NewInt32(ctx, 30), 0), "60");

    /* the call throws */
    val = JS_Eval(ctx, "caught()", 8, "<test>", JS_EVAL_TYPE_GLOBAL);
    CHECK(JS_IsException(val) && JS_IsSuspended(rt));
    CHECK_STRING(ctx, JS_ResumeWith(ctx, JS_NewString(ctx, "boom"), 1), "caught boom");

    /* through a bound function and a tail call */
    val = JS_Eval(ctx, "viaBound()", 10, "<test>", JS_EVAL_TYPE_GLOBAL);
    CHECK(JS_IsException(val) && JS_IsSuspended(rt));
    CHECK_STRING(ctx, JS_ResumeWith(ctx, JS_NewInt32(ctx, 1), 0), "2");
    val = JS_Eval(ctx, "tail()", 6, "<test>", JS_EVAL_TYPE_GLOBAL);
    CHECK(JS_IsException(val) && JS_IsSuspended(rt));
    CHECK_STRING(ctx, JS_ResumeWith(ctx, JS_NewInt32(ctx, 5), 0), "5");

    /* calls which cannot be suspended throw a TypeError */
    CHECK_STRING(ctx, eval(ctx,
        "const errors = [];"
        "function attempt(f) { try { f(); errors.push('no error'); }"
        "  catch (e) { errors.push(e instanceof TypeError); } }"
        "attempt(() => [1].forEach(() => pause()));"
        "attempt(() => ({ get x() { return pause(); } }).x);"
        "attempt(() => pause.call(null));"
        "attempt(() => eval('pause()'));"
        "attempt(() => Array.from({ next: pauseNext, [Symbol.iterator]() { return this; } }));"
        "errors.join()"), "true,true,true,true,true");
    {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue pause = JS_GetPropertyStr(ctx, global, "pause");
        JSValue probe;
        val = JS_Call(ctx, pause, JS_UNDEFINED, 0, NULL);
        CHECK(JS_IsException(val) && !JS_IsSuspended(rt) && !JS_IsSuspendException(ctx));
        JS_FreeValue(ctx, JS_GetException(ctx));
        /* JS run by host code reached from a host call (here an exotic
           property read) is not entered from the host */
        JS_NewClassID(&probe_class_id);
        JS_NewClass(rt, probe_class_id, &probe_class);
        probe = JS_NewObjectClass(ctx, probe_class_id);
        val = JS_GetPropertyStr(ctx, probe, "x");
        CHECK(JS_IsException(val) && !JS_IsSuspended(rt) && !JS_IsSuspendException(ctx));
        JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, probe);
        JS_FreeValue(ctx, pause);
        JS_FreeValue(ctx, global);
    }
    /* only one suspended execution */
    val = JS_Eval(ctx, "work(1)", 7, "<test>", JS_EVAL_TYPE_GLOBAL);
    CHECK(JS_IsException(val) && JS_IsSuspended(rt));
    CHECK_STRING(ctx, eval(ctx, "try { pause(); 'no error' } catch (e) { e.message }"),
                 "this call cannot be suspended");
    CHECK_STRING(ctx, JS_ResumeWith(ctx, JS_NewInt32(ctx, 4), 0), "8");

    /* JS_FreeRuntime() releases a suspended call */
    val = JS_Eval(ctx, "work(2)", 7, "<test>", JS_EVAL_TYPE_GLOBAL);
    CHECK(JS_IsException(val) && JS_IsSuspended(rt));
    free_context_and_runtime(ctx);
}

/* the string conversion of the value of 'src' (malloc'd) */
static char *eval_string(JSContext *ctx, const char *src)
{
    JSValue val = eval(ctx, src);
    const char *str = JS_ToCString(ctx, val);
    char *copy = strdup(str ? str : "<exception>");
    JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, val);
    return copy;
}

/* A base is reproducible: runtimes set up the same way write the same
   base, so a host can rebuild the base of its deltas instead of storing
   it. The state of Math.random() is not part of a base: full snapshots
   and deltas carry it. */
static void test_reproducible_base(void)
{
    JSContext *ctx1 = new_context(new_runtime());
    JSContext *ctx2 = new_context(new_runtime());
    JSContext *ctx3;
    uint8_t *base1, *base2, *delta, *full;
    size_t base1_size, base2_size, delta_size, full_size;
    char *expected;

    /* the same lazily instantiated builtins, but other Math.random()
       states (even if both runtimes got the same seed) */
    JS_FreeValue(ctx1, eval(ctx1, "Math.random()"));
    JS_FreeValue(ctx2, eval(ctx2, "Math.random(); Math.random()"));
    base1 = write_snapshot_flags(ctx1, &base1_size, NULL, 0, JS_SNAPSHOT_BASE);
    base2 = write_snapshot_flags(ctx2, &base2_size, NULL, 0, JS_SNAPSHOT_BASE);
    CHECK(base1 && base2 && base1_size == base2_size &&
          !memcmp(base1, base2, base1_size));
    free_context_and_runtime(ctx2);

    /* a delta applies to the base written by the other runtime and
       continues the Math.random() sequence */
    JS_FreeValue(ctx1, eval(ctx1, "var twice = x => 2 * x; Math.random()"));
    delta = write_snapshot_flags(ctx1, &delta_size, NULL, 0, JS_SNAPSHOT_DELTA);
    expected = eval_string(ctx1, "twice(21) + ' ' + Math.random()");
    if (base2 && delta && (ctx3 = read_snapshot(base2, base2_size, delta, delta_size))) {
        CHECK_STRING(ctx3, eval(ctx3, "twice(21) + ' ' + Math.random()"), expected);
        free_context_and_runtime(ctx3);
    }
    free(expected);

    /* so does a full snapshot */
    full = write_snapshot(ctx1, &full_size, NULL, 0);
    expected = eval_string(ctx1, "Math.random()");
    if (full && (ctx3 = read_snapshot(full, full_size, NULL, 0))) {
        CHECK_STRING(ctx3, eval(ctx3, "Math.random()"), expected);
        free_context_and_runtime(ctx3);
    }
    free(expected);
    free_context_and_runtime(ctx1);
    free(base1);
    free(base2);
    free(delta);
    free(full);
}

/* Incremental snapshots: natives, global changes and an application
   script make the base; the following snapshots are small deltas of it,
   also when written by a runtime restored from the base and a delta. */
static void test_incremental(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_context(rt), *ctx2;
    JSSnapshotResult res;
    uint8_t *base, *d0, *d1, *d2, *other;
    size_t base_size, d0_size, d1_size, d2_size, other_size, len;
    char err[256];

    JS_FreeValue(ctx, eval(ctx,
        "globalThis.config = { name: 'tenant-1', limits: { max: 100 } };"
        "Object.freeze(Array.prototype);"
        "var store = { items: [], counter: newCounter(),"
        "  log(s) { this.items.push(s); return this.items.length; } };"
        "store.counter.inc = inc;"
        "function handle(x) { return store.log('item ' + x) * 100 + store.counter.inc(); }"
        "var lib = {};"
        "for (let i = 0; i < 200; i++) lib['f' + i] = new Function('x', 'return x + ' + i);"));
    base = write_snapshot_flags(ctx, &base_size, NULL, 0, JS_SNAPSHOT_BASE);
    CHECK(base != NULL);

    /* nothing changed: only the header and the roots */
    d0 = write_snapshot_flags(ctx, &d0_size, NULL, 0, JS_SNAPSHOT_DELTA);
    CHECK(d0 != NULL && d0_size < 128);

    CHECK(eval_int(ctx, "handle(1); handle(2)") == 202);
    d1 = write_snapshot_flags(ctx, &d1_size, NULL, 0, JS_SNAPSHOT_DELTA);
    CHECK(d1 != NULL && d1_size * 10 < base_size);
    printf("incremental: base %zu bytes, unchanged delta %zu bytes, delta after 2 calls %zu bytes\n",
           base_size, d0_size, d1_size);
    free_context_and_runtime(ctx);
    if (!base || !d0 || !d1)
        return;

    /* base + d0 is the base state */
    ctx2 = read_snapshot(base, base_size, d0, d0_size);
    if (ctx2) {
        CHECK(eval_int(ctx2, "store.items.length + config.limits.max") == 100);
        free_context_and_runtime(ctx2);
    }

    /* base + d1, then a delta written by the restored runtime */
    ctx2 = read_snapshot(base, base_size, d1, d1_size);
    if (!ctx2)
        return;
    CHECK(eval_int(ctx2, "store.items.length") == 2);
    CHECK(eval_int(ctx2, "lib.f150(1) + config.limits.max") == 251);
    CHECK(eval_int(ctx2, "Object.isFrozen(Array.prototype) ? 1 : 0") == 1);
    CHECK(eval_int(ctx2, "handle(3)") == 303);
    d2 = write_snapshot_flags(ctx2, &d2_size, NULL, 0, JS_SNAPSHOT_DELTA);
    CHECK(d2 != NULL && d2_size * 10 < base_size);
    free_context_and_runtime(ctx2);
    if (!d2)
        return;
    ctx2 = read_snapshot(base, base_size, d2, d2_size);
    if (ctx2) {
        CHECK(eval_int(ctx2, "store.items.join(',') === 'item 1,item 2,item 3' ? 1 : 0") == 1);
        CHECK(eval_int(ctx2, "handle(4)") == 404);
        free_context_and_runtime(ctx2);
    }

    /* a base cell that is no longer reachable can still be modified:
       detaching the buffer clears its unreachable view */
    rt = new_runtime();
    ctx = new_context(rt);
    JS_FreeValue(ctx, eval(ctx, "var buffer = new ArrayBuffer(8); var view = new Uint8Array(buffer);"));
    other = write_snapshot_flags(ctx, &other_size, NULL, 0, JS_SNAPSHOT_BASE);
    JS_FreeValue(ctx, eval(ctx, "view = null; buffer.transfer(0);"));
    free(d0);
    d0 = write_snapshot_flags(ctx, &d0_size, NULL, 0, JS_SNAPSHOT_DELTA);
    free_context_and_runtime(ctx);
    ctx2 = read_snapshot(other, other_size, d0, d0_size);
    if (ctx2) {
        CHECK(eval_int(ctx2, "buffer.detached ? 1 : 0") == 1);
        free_context_and_runtime(ctx2);
    }
    free(other);
    other = NULL;

    /* every truncation of a delta is rejected cleanly */
    for(len = 0; len < d2_size; len++) {
        rt = new_runtime();
        CHECK(JS_ReadSnapshot(rt, base, base_size, d2, len, &host, &res, err, sizeof(err)) < 0);
        JS_FreeRuntime(rt);
    }

    /* a delta only applies to its base */
    rt = new_runtime();
    ctx = new_context(rt);
    other = write_snapshot_flags(ctx, &other_size, NULL, 0, JS_SNAPSHOT_BASE);
    free_context_and_runtime(ctx);
    rt = new_runtime();
    CHECK(JS_ReadSnapshot(rt, other, other_size, d2, d2_size, &host, &res, err, sizeof(err)) < 0);
    CHECK(strstr(err, "base") != NULL);
    JS_FreeRuntime(rt);
    /* ... and is not a full snapshot */
    rt = new_runtime();
    CHECK(JS_ReadSnapshot(rt, d2, d2_size, NULL, 0, &host, &res, err, sizeof(err)) < 0);
    JS_FreeRuntime(rt);
    /* a self-contained snapshot is not a base */
    rt = new_runtime();
    ctx = new_context(rt);
    free(other);
    other = write_snapshot(ctx, &other_size, NULL, 0);
    free_context_and_runtime(ctx);
    rt = new_runtime();
    CHECK(JS_ReadSnapshot(rt, other, other_size, d2, d2_size, &host, &res, err, sizeof(err)) < 0);
    JS_FreeRuntime(rt);

    /* deltas need a base; flags cannot be combined */
    rt = new_runtime();
    ctx = new_context(rt);
    CHECK(JS_WriteSnapshot(ctx, &len, &host, NULL, 0, JS_SNAPSHOT_DELTA) == NULL);
    JS_FreeValue(ctx, JS_GetException(ctx));
    CHECK(JS_WriteSnapshot(ctx, &len, &host, NULL, 0, JS_SNAPSHOT_BASE | JS_SNAPSHOT_DELTA) == NULL);
    JS_FreeValue(ctx, JS_GetException(ctx));
    free(write_snapshot_flags(ctx, &len, NULL, 0, JS_SNAPSHOT_BASE));
    JS_DropSnapshotBase(rt);
    CHECK(JS_WriteSnapshot(ctx, &len, &host, NULL, 0, JS_SNAPSHOT_DELTA) == NULL);
    JS_FreeValue(ctx, JS_GetException(ctx));
    free_context_and_runtime(ctx);

    free(base);
    free(d0);
    free(d1);
    free(d2);
    free(other);
}

/* touch every property reachable from the global object so that every
   lazily instantiated builtin exists, then hibernate: all the code
   pointers of the engine must be in the registry */
static void test_registry_complete(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_context(rt);
    JSSnapshotResult res;
    char err[256];
    size_t size;
    uint8_t *buf;

    JS_FreeValue(ctx, eval(ctx,
        "var seen = new Set();"
        "(function walk(o) {"
        "  if ((typeof o !== 'object' && typeof o !== 'function') || o === null || seen.has(o)) return;"
        "  seen.add(o);"
        "  for (const k of Reflect.ownKeys(o)) {"
        "    let d; try { d = Object.getOwnPropertyDescriptor(o, k); } catch (e) { continue; }"
        "    if (!d) continue;"
        "    walk(d.value); walk(d.get); walk(d.set);"
        "  }"
        "  walk(Object.getPrototypeOf(o));"
        "})(globalThis);"
        "seen = undefined;"));
    buf = write_snapshot(ctx, &size, NULL, 0);
    CHECK(buf != NULL);
    printf("registry: fully instantiated context = %zu bytes\n", size);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    if (!buf)
        return;

    rt = new_runtime();
    CHECK(JS_ReadSnapshot(rt, buf, size, NULL, 0, &host, &res, err, sizeof(err)) == 0);
    free(buf);
    if (res.context_count != 1) {
        fprintf(stderr, "read: %s\n", err);
        failures++;
        JS_FreeRuntime(rt);
        return;
    }
    ctx = res.contexts[0];
    JS_FreeSnapshotResult(rt, &res);
    CHECK(eval_int(ctx,
        "[1,2,3].map(x => x * 2).reduce((a, b) => a + b, 0) +"
        "Math.max(...[4, 5]) + JSON.parse('{\"a\":7}').a +"
        "'abc'.toUpperCase().charCodeAt(0) + Number.parseInt('10') +"
        "Object.entries({x: 1}).length + new Date(0).getUTCFullYear() +"
        "[...'hey'].length + Reflect.ownKeys({a: 1}).length") ==
          12 + 5 + 7 + 65 + 10 + 1 + 1970 + 3 + 1);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* fresh context: size of the baseline snapshot */
static void test_fresh_size(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_context(rt);
    size_t size;
    uint8_t *buf = write_snapshot(ctx, &size, NULL, 0);
    CHECK(buf != NULL);
    printf("fresh context = %zu bytes\n", size);
    free(buf);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static void test_host_class_and_roots(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_context(rt);
    JSSnapshotResult res;
    JSValue roots[2], fn, ret;
    char err[256];
    size_t size;
    uint8_t *buf;

    JS_FreeValue(ctx, eval(ctx, "var c = newCounter(); c.inc = inc; c.inc(); c.inc();"));
    roots[0] = eval(ctx, "(x) => c.inc() * 100 + x");
    roots[1] = JS_NewInt32(ctx, 7); /* a non-cell root */
    buf = write_snapshot(ctx, &size, roots, 2);
    CHECK(buf != NULL);
    JS_FreeValue(ctx, roots[0]);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    if (!buf)
        return;

    rt = new_runtime();
    CHECK(JS_ReadSnapshot(rt, buf, size, NULL, 0, &host, &res, err, sizeof(err)) == 0);
    free(buf);
    CHECK(res.context_count == 1 && res.root_count == 2);
    if (res.context_count != 1 || res.root_count != 2) {
        fprintf(stderr, "read: %s\n", err);
        JS_FreeRuntime(rt);
        return;
    }
    ctx = res.contexts[0];
    fn = res.roots[0];
    CHECK(JS_VALUE_GET_TAG(res.roots[1]) == JS_TAG_INT && JS_VALUE_GET_INT(res.roots[1]) == 7);
    JS_FreeSnapshotResult(rt, &res);
    {
        JSValue arg = JS_NewInt32(ctx, 5);
        ret = JS_Call(ctx, fn, JS_UNDEFINED, 1, &arg);
    }
    CHECK(JS_VALUE_GET_TAG(ret) == JS_TAG_INT && JS_VALUE_GET_INT(ret) == 305);
    CHECK(eval_int(ctx, "c.inc()") == 4);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, fn);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* the target of a WeakRef is only reachable through a cycle: a GC run
   by save_opaque() after the target was queued must not free it */
static void test_gc_during_write(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_context(rt);
    JSSnapshotResult res;
    char err[256];
    size_t size;
    uint8_t *buf;

    JS_FreeValue(ctx, eval(ctx,
        "var wr = new WeakRef((() => { const o = { tag: 'cycle' }; o.self = o; return o; })());"
        "var c = newCounter();"));
    buf = write_snapshot(ctx, &size, NULL, 0);
    CHECK(buf != NULL);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    if (!buf)
        return;
    rt = new_runtime();
    CHECK(JS_ReadSnapshot(rt, buf, size, NULL, 0, &host, &res, err, sizeof(err)) == 0);
    free(buf);
    if (res.context_count != 1) {
        fprintf(stderr, "read: %s\n", err);
        failures++;
        JS_FreeRuntime(rt);
        return;
    }
    ctx = res.contexts[0];
    JS_FreeSnapshotResult(rt, &res);
    /* either kept (as written) or collected: never a dangling object */
    CHECK(eval_int(ctx, "wr.deref() === undefined || wr.deref().self.tag === 'cycle' ? 1 : 0") == 1);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* objects shared between two realms keep their identity and realm */
static void test_two_realms(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *a = new_context(rt), *b = new_context(rt);
    JSSnapshotResult res;
    JSValue ga, gb;
    char err[256];
    size_t size;
    uint8_t *buf;

    JS_FreeValue(a, eval(a, "var shared = { arr: [1, 2], f() { return new Array(3); } };"));
    ga = JS_GetGlobalObject(a);
    gb = JS_GetGlobalObject(b);
    JS_SetPropertyStr(b, gb, "fromA", JS_GetPropertyStr(a, ga, "shared"));
    JS_SetPropertyStr(b, gb, "aGlobal", JS_DupValue(a, ga));
    JS_FreeValue(a, ga);
    JS_FreeValue(b, gb);
    buf = write_snapshot(b, &size, NULL, 0);
    CHECK(buf != NULL);
    JS_FreeContext(a);
    JS_FreeContext(b);
    JS_FreeRuntime(rt);
    if (!buf)
        return;

    rt = new_runtime();
    CHECK(JS_ReadSnapshot(rt, buf, size, NULL, 0, &host, &res, err, sizeof(err)) == 0);
    free(buf);
    CHECK(res.context_count == 2);
    if (res.context_count != 2) {
        fprintf(stderr, "read: %s\n", err);
        JS_FreeRuntime(rt);
        return;
    }
    a = res.contexts[0];
    b = res.contexts[1];
    JS_FreeSnapshotResult(rt, &res);
    /* same object seen from both realms */
    JS_FreeValue(a, eval(a, "shared.arr.push(3)"));
    CHECK(eval_int(b, "fromA.arr.length") == 3);
    CHECK(eval_int(b, "fromA === aGlobal.shared ? 1 : 0") == 1);
    /* arrays created by A's function belong to A's realm */
    CHECK(eval_int(b, "fromA.f() instanceof Array ? 1 : 0") == 0);
    CHECK(eval_int(b, "fromA.f() instanceof aGlobal.Array ? 1 : 0") == 1);
    CHECK(eval_int(b, "aGlobal.Array === Array ? 1 : 0") == 0);
    JS_FreeContext(a);
    JS_FreeContext(b);
    JS_FreeRuntime(rt);
}

static void test_errors(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_context(rt);
    JSSnapshotResult res;
    JSSnapshotHost other_host = host;
    char err[256];
    size_t size, len;
    uint8_t *buf;

    JS_FreeValue(ctx, eval(ctx,
        "var o = { s: 'x'.repeat(100), f() { return 1 }, big: 2n ** 100n, sym: Symbol('s') };"
        "var mk = (n) => () => n++; var cl = mk(3); cl();"
        "var b = function (a, c) { return this.k + a + c; }.bind({ k: 1 }, 2);"
        "var arr = [1, 'two', [3], { four: 4 }]; arr.length = 10;"
        "class K { #p = 1; get p() { return this.#p; } } var k = new K();"
        "var re = /x+/g; var d = new Date(0); var c = newCounter(); c.inc = inc; c.inc();"
        "var args = (function (x) { return arguments; })(1, 2);"));
    buf = write_snapshot(ctx, &size, NULL, 0);
    CHECK(buf != NULL);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    if (!buf)
        return;

    /* every truncation is rejected and leaves a runtime that can be freed */
    for(len = 0; len < size; len += (len < 4096 ? 1 : 1 + len / 256)) {
        rt = new_runtime();
        CHECK(JS_ReadSnapshot(rt, buf, len, NULL, 0, &host, &res, err, sizeof(err)) < 0);
        CHECK(err[0] != '\0');
        JS_FreeRuntime(rt);
    }

    /* different host natives: rejected by the signature */
    other_host.func_list_count = 0;
    rt = new_runtime();
    CHECK(JS_ReadSnapshot(rt, buf, size, NULL, 0, &other_host, &res, err, sizeof(err)) < 0);
    CHECK(strstr(err, "build") != NULL);
    JS_FreeRuntime(rt);

    /* the runtime must be empty */
    rt = new_runtime();
    ctx = JS_NewContext(rt);
    CHECK(JS_ReadSnapshot(rt, buf, size, NULL, 0, &host, &res, err, sizeof(err)) < 0);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    /* host class not registered */
    rt = JS_NewRuntime();
    CHECK(JS_ReadSnapshot(rt, buf, size, NULL, 0, &host, &res, err, sizeof(err)) < 0);
    CHECK(strstr(err, "class") != NULL);
    JS_FreeRuntime(rt);
    free(buf);

    /* snapshot while JS is running: a host function calling it fails */
    rt = new_runtime();
    ctx = new_context(rt);
    {
        JSValue global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "trySnapshot",
                          JS_NewCFunction(ctx, js_try_snapshot, "trySnapshot", 0));
        JS_FreeValue(ctx, global);
    }
    CHECK(eval_int(ctx, "trySnapshot() ? 1 : 0") == 1);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static JSValue closure_get(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv, int magic,
                           void *opaque)
{
    return JS_NewInt32(ctx, *(int *)opaque + magic);
}

/* the write of a C closure (JS_NewCClosure()) fails: its opaque pointer
   is host state */
static int write_fails_on_closure(JSContext *ctx, int flags)
{
    size_t size;
    uint8_t *buf = JS_WriteSnapshot(ctx, &size, &host, NULL, 0, flags);
    JSValue exc;
    const char *str;
    int ret;

    if (buf) {
        js_free(ctx, buf);
        return 0;
    }
    exc = JS_GetException(ctx);
    str = JS_ToCString(ctx, exc);
    ret = str && strstr(str, "TypeError") && strstr(str, "C closure");
    JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, exc);
    return ret;
}

static void test_c_closure(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_context(rt), *ctx2;
    JSValue global = JS_GetGlobalObject(ctx);
    static int state = 40;
    uint8_t *buf;
    size_t size;

    buf = write_snapshot_flags(ctx, &size, NULL, 0, JS_SNAPSHOT_BASE);
    CHECK(buf != NULL);
    JS_SetPropertyStr(ctx, global, "cl",
                      JS_NewCClosure(ctx, closure_get, "cl", NULL, 0, 2, &state));
    CHECK(eval_int(ctx, "cl()") == 42);
    CHECK(write_fails_on_closure(ctx, 0));
    CHECK(write_fails_on_closure(ctx, JS_SNAPSHOT_DELTA));
    /* only reachable from a closure variable */
    JS_FreeValue(ctx, eval(ctx, "var get = ((f) => () => f())(cl); delete globalThis.cl;"));
    CHECK(write_fails_on_closure(ctx, 0));
    CHECK(eval_int(ctx, "get()") == 42);
    /* the runtime still works and can be saved once the closure is gone */
    JS_FreeValue(ctx, eval(ctx, "get = null"));
    JS_RunGC(rt);
    free(buf);
    buf = write_snapshot(ctx, &size, NULL, 0);
    CHECK(buf != NULL);
    if (buf && (ctx2 = read_snapshot(buf, size, NULL, 0))) {
        CHECK(eval_int(ctx2, "typeof cl === 'undefined' && get === null ? 1 : 0") == 1);
        free_context_and_runtime(ctx2);
    }
    free(buf);
    JS_FreeValue(ctx, global);
    free_context_and_runtime(ctx);
}

/* roots: private symbol, private symbol without description, object */
static void check_private_symbols(JSContext *ctx, JSValue *roots)
{
    JSAtom a0 = JS_ValueToAtom(ctx, roots[0]);
    JSAtom a1 = JS_ValueToAtom(ctx, roots[1]);
    JSValue v, fresh;

    CHECK(JS_IsSymbol(roots[0]) && JS_IsSymbol(roots[1]));
    v = JS_GetProperty(ctx, roots[2], a0);
    CHECK(JS_VALUE_GET_TAG(v) == JS_TAG_INT && JS_VALUE_GET_INT(v) == 42);
    v = JS_GetProperty(ctx, roots[2], a1);
    CHECK(JS_VALUE_GET_TAG(v) == JS_TAG_INT && JS_VALUE_GET_INT(v) == 43);
    CHECK_STRING(ctx, JS_GetPropertyStr(ctx, roots[0], "description"), "hidden");
    v = JS_GetPropertyStr(ctx, roots[1], "description");
    CHECK(JS_IsUndefined(v));
    /* still hidden from JS, and distinct from a new private symbol */
    CHECK(eval_int(ctx, "Reflect.ownKeys(o).length === 3 && "
                   "Object.getOwnPropertySymbols(o).length === 1 ? 1 : 0") == 1);
    fresh = JS_NewPrivateSymbol(ctx, "hidden");
    CHECK(!JS_StrictEq(ctx, fresh, roots[0]));
    JS_FreeValue(ctx, fresh);
    JS_FreeAtom(ctx, a0);
    JS_FreeAtom(ctx, a1);
}

static JSContext *read_with_roots(const uint8_t *buf, size_t size,
                                  const uint8_t *delta, size_t delta_size,
                                  JSValue *roots, int n)
{
    JSRuntime *rt = new_runtime();
    JSSnapshotResult res;
    JSContext *ctx;
    char err[256];
    int i;

    if (JS_ReadSnapshot(rt, buf, size, delta, delta_size, &host, &res, err, sizeof(err)) ||
        res.root_count != n) {
        fprintf(stderr, "read: %s\n", err);
        failures++;
        JS_FreeRuntime(rt);
        return NULL;
    }
    ctx = res.contexts[0];
    for(i = 0; i < n; i++)
        roots[i] = res.roots[i];
    JS_FreeSnapshotResult(rt, &res);
    return ctx;
}

static void free_roots(JSContext *ctx, JSValue *roots, int n)
{
    int i;
    for(i = 0; i < n; i++)
        JS_FreeValue(ctx, roots[i]);
}

static void test_private_symbols(void)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_context(rt), *ctx2;
    JSValue roots[3], read_roots[3];
    JSAtom atom;
    uint8_t *base, *delta, *full;
    size_t base_size, delta_size, full_size;

    roots[2] = eval(ctx, "globalThis.o = { a: 1, [Symbol('pub')]: 2 }; Reflect.defineProperty(o, 'pub', { value: 3, enumerable: true }); o");
    base = write_snapshot_flags(ctx, &base_size, NULL, 0, JS_SNAPSHOT_BASE);
    roots[0] = JS_NewPrivateSymbol(ctx, "hidden");
    roots[1] = JS_NewPrivateSymbol(ctx, NULL);
    atom = JS_ValueToAtom(ctx, roots[0]);
    JS_DefinePropertyValue(ctx, roots[2], atom, JS_NewInt32(ctx, 42), JS_PROP_C_W_E);
    JS_FreeAtom(ctx, atom);
    atom = JS_ValueToAtom(ctx, roots[1]);
    JS_DefinePropertyValue(ctx, roots[2], atom, JS_NewInt32(ctx, 43), JS_PROP_C_W_E);
    JS_FreeAtom(ctx, atom);
    check_private_symbols(ctx, roots);

    /* the private atom kind survives full snapshots and deltas */
    full = write_snapshot(ctx, &full_size, roots, 3);
    delta = write_snapshot_flags(ctx, &delta_size, roots, 3, JS_SNAPSHOT_DELTA);
    CHECK(base && full && delta);
    if (full && (ctx2 = read_with_roots(full, full_size, NULL, 0, read_roots, 3))) {
        check_private_symbols(ctx2, read_roots);
        free_roots(ctx2, read_roots, 3);
        free_context_and_runtime(ctx2);
    }
    if (base && delta &&
        (ctx2 = read_with_roots(base, base_size, delta, delta_size, read_roots, 3))) {
        check_private_symbols(ctx2, read_roots);
        free_roots(ctx2, read_roots, 3);
        free_context_and_runtime(ctx2);
    }
    free(base);
    free(delta);
    free(full);
    free_roots(ctx, roots, 3);
    free_context_and_runtime(ctx);
}

static double eval_double(JSContext *ctx, const char *src)
{
    JSValue val = eval(ctx, src);
    double d = -1;
    JS_ToFloat64(ctx, &d, val);
    JS_FreeValue(ctx, val);
    return d;
}

/* performance.now() continues from its value at the write in the
   restored runtime and performance.timeOrigin is kept; a base read
   alone starts a new time origin */
static void test_performance(void)
{
    JSContext *ctx = new_context(new_runtime());
    JSContext *ctx_other = new_context(new_runtime());
    JSContext *ctx2;
    uint8_t *base, *base_other, *delta, *full;
    size_t base_size, base_other_size, delta_size, full_size;
    double origin, now, origin2;

    /* no clock value in the heap: identical bases */
    JS_FreeValue(ctx, eval(ctx, "performance.now() + performance.timeOrigin"));
    JS_FreeValue(ctx_other, eval(ctx_other, "for (let i = 0; i < 1e5; i++);"
                                 "performance.now() + performance.timeOrigin"));
    base = write_snapshot_flags(ctx, &base_size, NULL, 0, JS_SNAPSHOT_BASE);
    base_other = write_snapshot_flags(ctx_other, &base_other_size, NULL, 0, JS_SNAPSHOT_BASE);
    CHECK(base && base_other && base_size == base_other_size &&
          !memcmp(base, base_other, base_size));
    free_context_and_runtime(ctx_other);

    JS_FreeValue(ctx, eval(ctx, "var t0 = performance.now(); var origin = performance.timeOrigin;"));
    origin = eval_double(ctx, "origin");
    CHECK(origin > 1e12 && eval_double(ctx, "t0") >= 0);

    JS_FreeValue(ctx, eval(ctx, "var t1 = performance.now();"
                           "for (let i = 0; i < 1e5; i++); var t2 = performance.now();"));
    CHECK(eval_int(ctx, "t2 >= t1 && t1 >= t0 ? 1 : 0") == 1);
    full = write_snapshot(ctx, &full_size, NULL, 0);
    delta = write_snapshot_flags(ctx, &delta_size, NULL, 0, JS_SNAPSHOT_DELTA);
    now = eval_double(ctx, "performance.now()");
    free_context_and_runtime(ctx);
    CHECK(full && delta);
    if (full && (ctx2 = read_snapshot(full, full_size, NULL, 0))) {
        CHECK(eval_int(ctx2, "performance.now() >= t2 && "
                       "performance.timeOrigin === origin ? 1 : 0") == 1);
        /* not counted: the time between the write and the read */
        CHECK(eval_double(ctx2, "performance.now()") < now + 1000);
        free_context_and_runtime(ctx2);
    }
    if (base && delta && (ctx2 = read_snapshot(base, base_size, delta, delta_size))) {
        CHECK(eval_int(ctx2, "performance.now() >= t2 && "
                       "performance.timeOrigin === origin ? 1 : 0") == 1);
        free_context_and_runtime(ctx2);
    }
    if (base && (ctx2 = read_snapshot(base, base_size, NULL, 0))) {
        origin2 = eval_double(ctx2, "performance.timeOrigin");
        CHECK(origin2 >= origin);
        CHECK(eval_double(ctx2, "performance.now()") < 1000);
        free_context_and_runtime(ctx2);
    }
    free(base);
    free(base_other);
    free(delta);
    free(full);
}

int main(void)
{
    JS_NewClassID(&counter_class_id);
    test_fresh_size();
    test_registry_complete();
    test_host_class_and_roots();
    test_gc_during_write();
    test_two_realms();
    test_incremental();
    test_reproducible_base();
    test_suspend_call();
    test_errors();
    test_c_closure();
    test_private_symbols();
    test_performance();
    if (failures) {
        printf("FAIL test_snapshot_api: %d failures\n", failures);
        return 1;
    }
    printf("ok   test_snapshot_api\n");
    return 0;
}
