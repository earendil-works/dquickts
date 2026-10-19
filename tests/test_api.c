/*
 * QuickJS C API tests
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "../quickjs.h"

#define CHECK(cond) check(cond, #cond, __LINE__)
#define countof(x) (sizeof(x) / sizeof((x)[0]))

static void check(int cond, const char *msg, int line)
{
    if (!cond) {
        fprintf(stderr, "test_api:%d: check failed: %s\n", line, msg);
        exit(1);
    }
}

static JSValue eval_str(JSContext *ctx, const char *str)
{
    return JS_Eval(ctx, str, strlen(str), "<test>", JS_EVAL_TYPE_GLOBAL);
}

/* evaluate 'str' and return TRUE if it returns true */
static int eval_bool(JSContext *ctx, const char *str)
{
    JSValue v = eval_str(ctx, str);
    int ret;
    CHECK(!JS_IsException(v));
    ret = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return ret;
}

static void set_global(JSContext *ctx, const char *name, JSValue val)
{
    JSValue global = JS_GetGlobalObject(ctx);
    CHECK(JS_SetPropertyStr(ctx, global, name, val) >= 0);
    JS_FreeValue(ctx, global);
}

static void run_jobs(JSRuntime *rt)
{
    JSContext *ctx1;
    while (JS_IsJobPending(rt))
        CHECK(JS_ExecutePendingJob(rt, &ctx1) == 1);
}

static void test_objects(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue obj, values[20], arr;
    const char *names[20];
    char buf[20][8];
    JSAtom atoms[2];
    int64_t len;
    int i;

    atoms[0] = JS_NewAtom(ctx, "a");
    atoms[1] = JS_NewAtom(ctx, "b");
    values[0] = JS_NewInt32(ctx, 1);
    values[1] = JS_NewString(ctx, "x");
    obj = JS_NewObjectFrom(ctx, 2, atoms, values);
    CHECK(!JS_IsException(obj));
    set_global(ctx, "o1", obj);
    CHECK(eval_bool(ctx, "JSON.stringify(o1) === '{\"a\":1,\"b\":\"x\"}'"));
    CHECK(eval_bool(ctx, "Object.getOwnPropertyDescriptor(o1, 'a').writable"));
    JS_FreeAtom(ctx, atoms[0]);
    /* JS_DupAtomRT() keeps the atom alive after JS_FreeAtom() */
    CHECK(JS_DupAtomRT(rt, atoms[1]) == atoms[1]);
    JS_FreeAtom(ctx, atoms[1]);
    JS_FreeAtom(ctx, atoms[1]);

    /* more names than the stack buffer of JS_NewObjectFromStr() */
    for(i = 0; i < 20; i++) {
        snprintf(buf[i], sizeof(buf[i]), "p%d", i);
        names[i] = buf[i];
        values[i] = JS_NewInt32(ctx, i);
    }
    obj = JS_NewObjectFromStr(ctx, 20, names, values);
    CHECK(!JS_IsException(obj));
    set_global(ctx, "o2", obj);
    CHECK(eval_bool(ctx, "Object.keys(o2).length === 20 && o2.p19 === 19"));
    obj = JS_NewObjectFromStr(ctx, 0, NULL, NULL);
    CHECK(JS_IsObject(obj));
    JS_FreeValue(ctx, obj);

    for(i = 0; i < 3; i++)
        values[i] = JS_NewInt32(ctx, i * 10);
    arr = JS_NewArrayFrom(ctx, 3, values);
    CHECK(!JS_IsException(arr));
    CHECK(JS_IsArray(ctx, arr) == 1);
    CHECK(JS_GetLength(ctx, arr, &len) == 0 && len == 3);
    set_global(ctx, "a1", arr);
    CHECK(eval_bool(ctx, "a1.join() === '0,10,20'"));
    arr = JS_NewArrayFrom(ctx, 0, NULL);
    CHECK(JS_GetLength(ctx, arr, &len) == 0 && len == 0);
    JS_FreeValue(ctx, arr);

    obj = eval_str(ctx, "({ length: 2**53 + 10 })");
    CHECK(JS_GetLength(ctx, obj, &len) == 0 && len == (((int64_t)1 << 53) - 1));
    JS_FreeValue(ctx, obj);
    obj = eval_str(ctx, "({ get length() { throw 1 } })");
    CHECK(JS_GetLength(ctx, obj, &len) == -1);
    JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, obj);
}

static void test_numbers(JSContext *ctx)
{
    JSValue v;
    uint64_t u;
    double d;

    v = eval_str(ctx, "-1n");
    CHECK(JS_ToBigUint64(ctx, &u, v) == 0 && u == UINT64_MAX);
    JS_FreeValue(ctx, v);
    v = eval_str(ctx, "2n**64n + 5n");
    CHECK(JS_ToBigUint64(ctx, &u, v) == 0 && u == 5);
    JS_FreeValue(ctx, v);
    CHECK(JS_ToBigUint64(ctx, &u, JS_NewInt32(ctx, 1)) == -1);
    JS_FreeValue(ctx, JS_GetException(ctx));

    v = JS_NewUint64(ctx, 7);
    CHECK(JS_VALUE_GET_TAG(v) == JS_TAG_INT && JS_VALUE_GET_INT(v) == 7);
    v = JS_NewUint64(ctx, UINT64_MAX);
    CHECK(JS_ToFloat64(ctx, &d, v) == 0 && d == 18446744073709551616.0);
    v = JS_NewUint64(ctx, 0x80000000);
    CHECK(JS_ToFloat64(ctx, &d, v) == 0 && d == 2147483648.0);
}

static void test_predicates(JSContext *ctx)
{
    static const struct {
        const char *src;
        JS_BOOL (*func)(JSValueConst val);
    } tab[] = {
        { "new Date(0)", JS_IsDate },
        { "/a/g", JS_IsRegExp },
        { "new Map()", JS_IsMap },
        { "new Set()", JS_IsSet },
        { "new WeakRef({})", JS_IsWeakRef },
        { "new ArrayBuffer(1)", JS_IsArrayBuffer },
        { "(async function() {})", JS_IsAsyncFunction },
        { "Promise.resolve()", JS_IsPromise },
    };
    JSValue vals[countof(tab)];
    JSValue v;
    int i, j;

    for(i = 0; i < countof(tab); i++) {
        vals[i] = eval_str(ctx, tab[i].src);
        CHECK(!JS_IsException(vals[i]));
    }
    for(i = 0; i < countof(tab); i++) {
        for(j = 0; j < countof(tab); j++)
            CHECK(tab[i].func(vals[j]) == (i == j));
        CHECK(!tab[i].func(JS_UNDEFINED));
        CHECK(!tab[i].func(JS_NewInt32(ctx, 1)));
    }
    for(i = 0; i < countof(tab); i++)
        JS_FreeValue(ctx, vals[i]);
    v = eval_str(ctx, "new SharedArrayBuffer(1)");
    CHECK(!JS_IsArrayBuffer(v));
    JS_FreeValue(ctx, v);
    v = eval_str(ctx, "(async function*() {})");
    CHECK(!JS_IsAsyncFunction(v));
    JS_FreeValue(ctx, v);
}

static void test_typed_arrays(JSContext *ctx)
{
    JSValue arg, ta;
    int t;

    arg = JS_NewInt32(ctx, 4);
    for(t = JS_TYPED_ARRAY_UINT8C; t <= JS_TYPED_ARRAY_FLOAT64; t++) {
        ta = JS_NewTypedArray(ctx, 1, &arg, t);
        CHECK(!JS_IsException(ta));
        CHECK(JS_GetTypedArrayType(ta) == t);
        JS_FreeValue(ctx, ta);
    }
    ta = eval_str(ctx, "new DataView(new ArrayBuffer(1))");
    CHECK(JS_GetTypedArrayType(ta) == -1);
    JS_FreeValue(ctx, ta);
    CHECK(JS_GetTypedArrayType(JS_UNDEFINED) == -1);
}

static void test_errors(JSContext *ctx)
{
    JSValue e;

    CHECK(JS_IsException(JS_ThrowPlainError(ctx, "plain %d", 42)));
    e = JS_GetException(ctx);
    set_global(ctx, "e1", e);
    CHECK(eval_bool(ctx, "Object.getPrototypeOf(e1) === Error.prototype && "
                    "e1.message === 'plain 42' && Error.isError(e1)"));

    CHECK(JS_IsException(JS_ThrowDOMException(ctx, "NotFoundError", "no %s", "file")));
    e = JS_GetException(ctx);
    CHECK(JS_IsError(ctx, e));
    set_global(ctx, "e2", e);
    CHECK(eval_bool(ctx, "e2 instanceof DOMException && e2.name === 'NotFoundError' && "
                    "e2.message === 'no file' && e2.code === 8 && typeof e2.stack === 'string'"));
}

static void test_cstring(JSContext *ctx)
{
    JSValue v = JS_NewString(ctx, "hello");
    const char *s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    CHECK(s && !strcmp(s, "hello"));
    JS_FreeCStringRT(JS_GetRuntime(ctx), s);
    JS_FreeCStringRT(JS_GetRuntime(ctx), NULL);
}

static JSValue record_settle(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv, int magic)
{
    set_global(ctx, magic ? "rejected" : "fulfilled", JS_DupValue(ctx, argv[0]));
    return JS_NewInt32(ctx, 100 + magic);
}

static void test_promises(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue p, p2, on_ok, on_err, v;

    CHECK(!JS_IsJobPending(rt));
    CHECK(JS_GetPendingJobContext(rt) == NULL);

    v = JS_NewInt32(ctx, 5);
    p = JS_NewSettledPromise(ctx, 0, v);
    CHECK(JS_IsPromise(p));
    CHECK(JS_PromiseState(ctx, p) == JS_PROMISE_FULFILLED);
    v = JS_PromiseResult(ctx, p);
    CHECK(JS_VALUE_GET_INT(v) == 5);
    /* a promise is returned as is, like Promise.resolve() */
    p2 = JS_NewSettledPromise(ctx, 0, p);
    CHECK(JS_VALUE_GET_PTR(p2) == JS_VALUE_GET_PTR(p));
    JS_FreeValue(ctx, p2);

    on_ok = JS_NewCFunctionMagic(ctx, record_settle, "ok", 1, JS_CFUNC_generic_magic, 0);
    on_err = JS_NewCFunctionMagic(ctx, record_settle, "err", 1, JS_CFUNC_generic_magic, 1);
    p2 = JS_PromiseThen(ctx, p, on_ok, on_err);
    CHECK(JS_IsPromise(p2));
    CHECK(JS_GetPendingJobContext(rt) == ctx);
    run_jobs(rt);
    CHECK(JS_GetPendingJobContext(rt) == NULL);
    CHECK(JS_PromiseState(ctx, p2) == JS_PROMISE_FULFILLED);
    v = JS_PromiseResult(ctx, p2);
    CHECK(JS_VALUE_GET_INT(v) == 100);
    CHECK(eval_bool(ctx, "fulfilled === 5"));
    JS_FreeValue(ctx, p2);
    JS_FreeValue(ctx, p);

    p = JS_NewSettledPromise(ctx, 1, JS_NewInt32(ctx, 7));
    CHECK(JS_PromiseState(ctx, p) == JS_PROMISE_REJECTED);
    p2 = JS_PromiseThen(ctx, p, JS_UNDEFINED, on_err);
    run_jobs(rt);
    CHECK(eval_bool(ctx, "rejected === 7"));
    CHECK(JS_PromiseState(ctx, p2) == JS_PROMISE_FULFILLED);
    JS_FreeValue(ctx, p2);
    JS_FreeValue(ctx, p);

    /* neither 'then' nor the species constructor are used */
    p = eval_str(ctx, "var p3 = Promise.resolve(1); p3.then = () => { throw 1 };"
                 "p3.constructor = { [Symbol.species]: function() { throw 2 } }; p3");
    p2 = JS_PromiseThen(ctx, p, on_ok, JS_UNDEFINED);
    CHECK(JS_IsPromise(p2));
    run_jobs(rt);
    CHECK(JS_PromiseState(ctx, p2) == JS_PROMISE_FULFILLED);
    JS_FreeValue(ctx, p2);
    JS_FreeValue(ctx, p);

    /* not a promise */
    p2 = JS_PromiseThen(ctx, JS_UNDEFINED, on_ok, on_err);
    CHECK(JS_IsException(p2));
    JS_FreeValue(ctx, JS_GetException(ctx));

    JS_FreeValue(ctx, on_ok);
    JS_FreeValue(ctx, on_err);
}

/* promise hooks */

#define HOOK_LOG_SIZE 64

static struct {
    char log[HOOK_LOG_SIZE][32];
    int count;
    JSValue promises[HOOK_LOG_SIZE]; /* known promises, for the ids */
    int promise_count;
} hook_state;

static int hook_promise_id(JSContext *ctx, JSValueConst p)
{
    int i;
    for(i = 0; i < hook_state.promise_count; i++) {
        if (JS_VALUE_GET_PTR(hook_state.promises[i]) == JS_VALUE_GET_PTR(p))
            return i;
    }
    CHECK(hook_state.promise_count < HOOK_LOG_SIZE);
    hook_state.promises[hook_state.promise_count] = JS_DupValue(ctx, p);
    return hook_state.promise_count++;
}

static void promise_hook(JSContext *ctx, JSPromiseHookType type,
                         JSValueConst promise, JSValueConst parent_promise,
                         void *opaque)
{
    static const char * const names[] = { "init", "before", "after", "resolve" };
    char *s;
    int id, parent_id;

    CHECK(opaque == &hook_state);
    CHECK(JS_IsPromise(promise));
    CHECK(hook_state.count < HOOK_LOG_SIZE);
    s = hook_state.log[hook_state.count++];
    if (JS_IsUndefined(parent_promise)) {
        snprintf(s, 32, "%s:%d", names[type], hook_promise_id(ctx, promise));
    } else {
        CHECK(type == JS_PROMISE_HOOK_INIT);
        CHECK(JS_IsPromise(parent_promise));
        id = hook_promise_id(ctx, promise);
        parent_id = hook_promise_id(ctx, parent_promise);
        snprintf(s, 32, "%s:%d<%d", names[type], id, parent_id);
    }
}

static void hook_check(JSContext *ctx, const char *expected)
{
    char buf[HOOK_LOG_SIZE * 32];
    int i;

    buf[0] = '\0';
    for(i = 0; i < hook_state.count; i++) {
        if (i != 0)
            strcat(buf, " ");
        strcat(buf, hook_state.log[i]);
    }
    if (strcmp(buf, expected)) {
        fprintf(stderr, "test_api: promise hooks: got '%s', expected '%s'\n",
                buf, expected);
        exit(1);
    }
    for(i = 0; i < hook_state.promise_count; i++)
        JS_FreeValue(ctx, hook_state.promises[i]);
    hook_state.promise_count = 0;
    hook_state.count = 0;
}

static void test_promise_hooks(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx;
    JSValue v, p;

    JS_SetPromiseHook(rt, promise_hook, &hook_state);
    ctx = JS_NewContext(rt);

    v = eval_str(ctx, "new Promise(() => {}); undefined");
    JS_FreeValue(ctx, v);
    hook_check(ctx, "init:0");

    v = eval_str(ctx, "new Promise((res) => res(1)); undefined");
    JS_FreeValue(ctx, v);
    hook_check(ctx, "init:0 resolve:0");

    v = eval_str(ctx, "new Promise((res, rej) => rej(1)).catch(() => {}); undefined");
    JS_FreeValue(ctx, v);
    run_jobs(rt);
    /* rejections are not reported */
    hook_check(ctx, "init:0 init:1<0 resolve:1");

    v = eval_str(ctx, "globalThis.r = []; Promise.resolve(1).then(() => r.push(1)); undefined");
    JS_FreeValue(ctx, v);
    hook_check(ctx, "init:0 resolve:0 init:1<0");
    run_jobs(rt);
    hook_check(ctx, "resolve:0");

    /* thenable resolution: before and after hooks around its 'then' */
    v = eval_str(ctx, "new Promise(res => res({ then(f) { f(2) } })); undefined");
    JS_FreeValue(ctx, v);
    hook_check(ctx, "init:0");
    run_jobs(rt);
    hook_check(ctx, "before:0 resolve:0 after:0");

    /* C API */
    p = JS_NewSettledPromise(ctx, 0, JS_NewInt32(ctx, 1));
    hook_check(ctx, "init:0 resolve:0");
    v = JS_PromiseThen(ctx, p, JS_UNDEFINED, JS_UNDEFINED);
    hook_check(ctx, "init:0<1");
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, p);
    run_jobs(rt);
    hook_check(ctx, "resolve:0");

    /* the hook can be removed */
    JS_SetPromiseHook(rt, NULL, NULL);
    v = eval_str(ctx, "Promise.resolve(1).then(() => 2); undefined");
    JS_FreeValue(ctx, v);
    run_jobs(rt);
    hook_check(ctx, "");

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* regression test: Promise.withResolvers() freed the promise twice when
   running out of memory (quickjs-ng #1692) */
static void test_with_resolvers_oom(void)
{
    static const char src[] =
        "for (var i = 0; i < 4; i++) Promise.withResolvers();";
    JSRuntime *rt;
    JSContext *ctx;
    JSMemoryUsage stats;
    JSValue v;
    size_t limit;
    int ok = 0;

    for(limit = 0; limit < 16384 && !ok; limit += 8) {
        rt = JS_NewRuntime();
        ctx = JS_NewContext(rt);
        /* compiled without limit: only the execution runs out of memory */
        v = JS_Eval(ctx, src, strlen(src), "<test>",
                    JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        CHECK(!JS_IsException(v));
        JS_ComputeMemoryUsage(rt, &stats);
        JS_SetMemoryLimit(rt, stats.malloc_size + limit);
        v = JS_EvalFunction(ctx, v);
        ok = !JS_IsException(v);
        JS_FreeValue(ctx, v);
        if (!ok)
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_SetMemoryLimit(rt, 0);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt); /* asserts that no object is leaked */
    }
    CHECK(ok);
}

static int detect_module(const char *src)
{
    return JS_DetectModule(src, strlen(src));
}

static void test_detect_module(void)
{
    CHECK(detect_module("import { a } from 'x';"));
    CHECK(detect_module("/* c */ export default 1;"));
    CHECK(!detect_module("import('x').then(print);"));
    CHECK(!detect_module("var x = 1; // await"));
    /* top-level await (ng bug832.js) */
    CHECK(detect_module("const m = await import('./empty.js')\nprint(m)"));
    CHECK(detect_module("#!/usr/bin/env qjs\nfor await (const x of [/a/]) `${x}`;"));
    CHECK(detect_module("if (1) { await 0; }"));
    /* 'await' as an identifier: valid scripts stay scripts */
    CHECK(!detect_module("var await = 1; print(await);"));
    CHECK(!detect_module("function await(x) {}\nawait(1);"));
    CHECK(!detect_module("var r = await(1);")); /* also a valid module */
    CHECK(!detect_module("async function f() { await 1; }"));
    CHECK(!detect_module("var f = async () => await 1;"));
    /* neither a script nor a module */
    CHECK(!detect_module("await 1; let x = @;"));
}

static void test_version(void)
{
    CHECK(!strcmp(JS_GetVersion(), QJS_VERSION_STRING));
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d", QJS_VERSION_YEAR,
                 QJS_VERSION_MONTH, QJS_VERSION_DAY);
        CHECK(!strcmp(buf, QJS_VERSION_STRING));
    }
}

int main(void)
{
    JSRuntime *rt;
    JSContext *ctx;

    test_version();

    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);
    test_objects(ctx);
    test_numbers(ctx);
    test_predicates(ctx);
    test_typed_arrays(ctx);
    test_errors(ctx);
    test_cstring(ctx);
    test_promises(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    test_promise_hooks();
    test_with_resolvers_oom();
    test_detect_module();
    printf("test_api: OK\n");
    return 0;
}
