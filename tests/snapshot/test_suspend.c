/*
 * In-process suspension test host (JS_RequestSuspend() / JS_Resume()).
 *
 * Every script runs twice, each time in a fresh runtime: once without
 * interruption (reference run) and once with an interrupt handler that
 * requests a suspension at every call, the host resuming the execution
 * with JS_Resume() until it completes. The printed output and the
 * completion value must be identical. The suspended run also checks:
 * - no suspension happens inside a region marked by __enter()/__leave()
 *   (native callbacks, generators, async functions, jobs, getters...),
 * - the statistics collected by the script in __stats (incremented when
 *   __suspensions() changed while a given frame was live) prove that the
 *   interesting frames were really suspended,
 * - the GC and other JS code (including closures of the suspended
 *   frames) can run while an execution is suspended,
 * - every runtime frees all its memory (also when freed while suspended).
 * Other tests cover host calls (JS_Call(), JS_CallConstructor()),
 * JS_FreeRuntime() while suspended and out of memory while suspending.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdarg.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__) || defined(__GLIBC__)
#include <malloc.h>
#endif

#include "cutils.h"
#include "quickjs.h"

static int n_failures;
static int verbose;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
            n_failures++;                                       \
        }                                                       \
    } while (0)

/* host allocator counting the live blocks */

static long live_blocks;
/* live blocks larger than an allocator arena (large frames...) */
static long live_large_blocks;
#define LARGE_BLOCK_SIZE 4200
/* when set, the next allocation larger than fail_min_size fails */
static int fail_armed;
static size_t fail_min_size;

static size_t t_usable_size(const void *ptr)
{
#if defined(__APPLE__)
    return malloc_size(ptr);
#elif defined(__linux__) || defined(__GLIBC__)
    return malloc_usable_size((void *)ptr);
#else
    return 0;
#endif
}

static void *t_malloc(JSMallocState *s, size_t size)
{
    void *ptr;
    if (fail_armed && size > fail_min_size) {
        fail_armed = FALSE;
        return NULL;
    }
    ptr = malloc(size);
    if (!ptr)
        return NULL;
    live_blocks++;
    if (t_usable_size(ptr) >= LARGE_BLOCK_SIZE)
        live_large_blocks++;
    s->malloc_count++;
    s->malloc_size += t_usable_size(ptr);
    return ptr;
}

static void *t_calloc(JSMallocState *s, size_t count, size_t size)
{
    void *ptr = t_malloc(s, count * size);
    if (ptr)
        memset(ptr, 0, count * size);
    return ptr;
}

static void t_free(JSMallocState *s, void *ptr)
{
    if (!ptr)
        return;
    live_blocks--;
    if (t_usable_size(ptr) >= LARGE_BLOCK_SIZE)
        live_large_blocks--;
    s->malloc_count--;
    s->malloc_size -= t_usable_size(ptr);
    free(ptr);
}

static void *t_realloc(JSMallocState *s, void *ptr, size_t size)
{
    size_t old_size;
    if (!ptr)
        return size ? t_malloc(s, size) : NULL;
    if (size == 0) {
        t_free(s, ptr);
        return NULL;
    }
    old_size = t_usable_size(ptr);
    ptr = realloc(ptr, size);
    if (!ptr)
        return NULL;
    live_large_blocks += (t_usable_size(ptr) >= LARGE_BLOCK_SIZE) -
        (old_size >= LARGE_BLOCK_SIZE);
    s->malloc_size += t_usable_size(ptr) - old_size;
    return ptr;
}

static const JSMallocFunctions t_mf = {
    t_calloc, t_malloc, t_free, t_realloc, t_usable_size,
};

/* run state */

typedef struct {
    char *out;
    size_t out_len, out_size;
    int nosusp_depth; /* > 0 inside __enter() / __leave() */
    long handler_calls;
    long deferred; /* handler calls inside __enter() / __leave() */
    long suspensions;
    long side_evals;
} RunState;

static RunState *cur;

static void out_append(RunState *st, const char *s, size_t len)
{
    if (st->out_len + len + 1 > st->out_size) {
        st->out_size = (st->out_len + len + 1) * 2;
        st->out = realloc(st->out, st->out_size);
    }
    memcpy(st->out + st->out_len, s, len);
    st->out_len += len;
    st->out[st->out_len] = '\0';
}

static void out_str(RunState *st, const char *s)
{
    out_append(st, s, strlen(s));
}

static JSValue js_print(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    int i;
    size_t len;
    const char *str;
    for(i = 0; i < argc; i++) {
        if (i != 0)
            out_str(cur, " ");
        str = JS_ToCStringLen(ctx, &len, argv[i]);
        if (!str)
            return JS_EXCEPTION;
        out_append(cur, str, len);
        JS_FreeCString(ctx, str);
    }
    out_str(cur, "\n");
    return JS_UNDEFINED;
}

static JSValue js_enter(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    cur->nosusp_depth++;
    return JS_UNDEFINED;
}

static JSValue js_leave(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    cur->nosusp_depth--;
    return JS_UNDEFINED;
}

static JSValue js_suspensions(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    return JS_NewInt64(ctx, cur->suspensions);
}

static int interrupt_handler(JSRuntime *rt, void *opaque)
{
    RunState *st = opaque;
    st->handler_calls++;
    if (st->nosusp_depth > 0)
        st->deferred++;
    JS_RequestSuspend(rt);
    return 0;
}

static const char prelude[] =
    "var __stats = { deep: 0, leaf: 0, temps: 0, closure: 0, mapped: 0,"
    " finally: 0, throwAfterResume: 0, forof: 0, bound: 0, spread: 0,"
    " tail: 0, ctor: 0, derived: 0, after: 0, using: 0, dispose: 0 };\n";

static JSContext *new_context(JSRuntime *rt)
{
    JSContext *ctx;
    JSValue global, ret;

    ctx = JS_NewContext(rt);
    if (!ctx) {
        fprintf(stderr, "cannot create context\n");
        exit(1);
    }
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "print",
                      JS_NewCFunction(ctx, js_print, "print", 1));
    JS_SetPropertyStr(ctx, global, "__enter",
                      JS_NewCFunction(ctx, js_enter, "__enter", 0));
    JS_SetPropertyStr(ctx, global, "__leave",
                      JS_NewCFunction(ctx, js_leave, "__leave", 0));
    JS_SetPropertyStr(ctx, global, "__suspensions",
                      JS_NewCFunction(ctx, js_suspensions, "__suspensions", 0));
    JS_FreeValue(ctx, global);
    ret = JS_Eval(ctx, prelude, strlen(prelude), "<prelude>",
                  JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(ret)) {
        fprintf(stderr, "prelude failed\n");
        exit(1);
    }
    JS_FreeValue(ctx, ret);
    return ctx;
}

/* run other JS code while an execution is suspended: it cannot be
   suspended (only one suspended execution at a time) */
static void side_eval(JSContext *ctx, RunState *st)
{
    static const char src[] =
        "(function () { var s = 0; for (var i = 0; i < 30000; i++) s += i;"
        " return s; })()";
    static const char check_src[] =
        "typeof __check == 'function' ? __check() : 0";
    JSValue ret;
    int64_t v = 0;

    ret = JS_Eval(ctx, src, strlen(src), "<side>", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(ret), "side eval failed");
    JS_ToInt64(ctx, &v, ret);
    JS_FreeValue(ctx, ret);
    CHECK(v == 449985000, "side eval result %" PRId64, v);

    /* closures of suspended frames can be called while suspended */
    ret = JS_Eval(ctx, check_src, strlen(check_src), "<side>",
                  JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(ret), "__check() failed");
    v = -1;
    JS_ToInt64(ctx, &v, ret);
    JS_FreeValue(ctx, ret);
    CHECK(v == 0, "__check() = %" PRId64, v);

    CHECK(JS_IsSuspended(JS_GetRuntime(ctx)), "lost the suspended execution");
    st->side_evals++;
}

/* resume a suspended host call until it completes */
static JSValue finish(JSContext *ctx, RunState *st, JSValue ret)
{
    JSRuntime *rt = JS_GetRuntime(ctx);

    while (JS_IsSuspended(rt)) {
        CHECK(JS_IsException(ret), "suspended call did not return JS_EXCEPTION");
        CHECK(st->nosusp_depth == 0,
              "suspended inside a non suspendable region");
        st->suspensions++;
        if (st->suspensions % 4 == 0)
            JS_RunGC(rt);
        if (st->suspensions % 7 == 0)
            side_eval(ctx, st);
        ret = JS_Resume(ctx);
    }
    return ret;
}

/* text of a completion value; 'val' is freed */
static char *describe(JSContext *ctx, JSValue val)
{
    JSValue str_val;
    const char *str;
    char *res;
    const char *prefix = "";

    if (JS_IsException(val)) {
        val = JS_GetException(ctx);
        prefix = "EXC: ";
        str_val = JS_ToString(ctx, val);
    } else if (JS_IsObject(val)) {
        str_val = JS_JSONStringify(ctx, val, JS_UNDEFINED, JS_UNDEFINED);
    } else {
        str_val = JS_ToString(ctx, val);
    }
    JS_FreeValue(ctx, val);
    str = JS_ToCString(ctx, str_val);
    JS_FreeValue(ctx, str_val);
    res = malloc(strlen(prefix) + strlen(str ? str : "?") + 1);
    strcpy(res, prefix);
    strcat(res, str ? str : "?");
    JS_FreeCString(ctx, str);
    return res;
}

typedef struct {
    const char *name;
    const char *src;
    int min_suspensions;
    const char *stats[6]; /* __stats fields which must be > 0 */
    int expect_deferred; /* requests must have been deferred */
} TestCase;

typedef struct {
    char *out;
    char *completion;
    RunState st;
} RunResult;

typedef JSValue RunFunc(JSContext *ctx, const TestCase *tc);

static JSValue run_eval(JSContext *ctx, const TestCase *tc)
{
    return JS_Eval(ctx, tc->src, strlen(tc->src), tc->name,
                   JS_EVAL_TYPE_GLOBAL);
}

static void run(const TestCase *tc, RunFunc *func, int suspend,
                RunResult *res)
{
    JSRuntime *rt;
    JSContext *ctx, *ctx1;
    JSValue ret, global, stats, v;
    RunState *st = &res->st;
    int i, r, n;

    memset(res, 0, sizeof(*res));
    cur = st;
    rt = JS_NewRuntime2(&t_mf, NULL);
    ctx = new_context(rt);
    if (suspend)
        JS_SetInterruptHandler(rt, interrupt_handler, st);

    ret = func(ctx, tc);
    ret = finish(ctx, st, ret);

    /* pending jobs (promise reactions, async functions) are never
       suspended */
    for(;;) {
        r = JS_ExecutePendingJob(rt, &ctx1);
        CHECK(!JS_IsSuspended(rt), "%s: suspended inside a job", tc->name);
        if (r == 0)
            break;
        if (r < 0) {
            JSValue exc = JS_GetException(ctx);
            const char *str = JS_ToCString(ctx, exc);
            out_str(st, "job exception: ");
            out_str(st, str ? str : "?");
            out_str(st, "\n");
            JS_FreeCString(ctx, str);
            JS_FreeValue(ctx, exc);
        }
    }
    JS_SetInterruptHandler(rt, NULL, NULL);
    res->completion = describe(ctx, ret);

    if (suspend) {
        global = JS_GetGlobalObject(ctx);
        stats = JS_GetPropertyStr(ctx, global, "__stats");
        for(i = 0; i < 6 && tc->stats[i]; i++) {
            v = JS_GetPropertyStr(ctx, stats, tc->stats[i]);
            n = 0;
            JS_ToInt32(ctx, &n, v);
            JS_FreeValue(ctx, v);
            CHECK(n > 0, "%s: __stats.%s = %d", tc->name, tc->stats[i], n);
        }
        JS_FreeValue(ctx, stats);
        JS_FreeValue(ctx, global);
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    CHECK(live_blocks == 0, "%s: %ld blocks leaked", tc->name, live_blocks);
    live_blocks = 0;
    res->out = st->out ? st->out : strdup("");
    cur = NULL;
}

static void run_test(const TestCase *tc, RunFunc *func)
{
    RunResult ref, sus;

    run(tc, func, FALSE, &ref);
    run(tc, func, TRUE, &sus);
    CHECK(strcmp(ref.out, sus.out) == 0,
          "%s: output differs\n--- reference:\n%s--- suspended:\n%s",
          tc->name, ref.out, sus.out);
    CHECK(strcmp(ref.completion, sus.completion) == 0,
          "%s: completion differs: '%s' vs '%s'",
          tc->name, ref.completion, sus.completion);
    CHECK(sus.st.suspensions >= tc->min_suspensions,
          "%s: %ld suspensions, expected at least %d",
          tc->name, sus.st.suspensions, tc->min_suspensions);
    if (tc->expect_deferred) {
        CHECK(sus.st.deferred > 0, "%s: no deferred suspension request",
              tc->name);
    }
    printf("%-28s %4ld suspensions, %3ld deferred requests, %2ld side evals: %s\n",
           tc->name, sus.st.suspensions, sus.st.deferred, sus.st.side_evals,
           sus.completion);
    if (verbose)
        printf("%s", sus.out);
    free(ref.out);
    free(ref.completion);
    free(sus.out);
    free(sus.completion);
}

/* scripts */

static const TestCase tests[] = {
    {
        "toplevel_loop",
        "var s = 0;\n"
        "let t = 1;\n"
        "for (var i = 0; i < 300000; i++) {\n"
        "  s = (s + i * 7) % 1000003;\n"
        "  t = (t * 3 + s) % 65521;\n"
        "  if (i % 50000 == 0) print('i=' + i, s, t);\n"
        "}\n"
        "print('end', s, t);\n"
        "s * 1000 + t;\n",
        20, { NULL }, 0,
    },
    {
        "nested_calls",
        "function leaf(a, b) {\n"
        "  var c0 = __suspensions();\n"
        "  var acc = 0;\n"
        "  for (var k = 0; k < b; k++) acc = (acc * 31 + a + k) % 65521;\n"
        "  if (__suspensions() != c0) __stats.leaf++;\n"
        "  return acc;\n"
        "}\n"
        "function mid(x, depth) {\n"
        "  var local = x * 2, str = 'm' + depth;\n"
        "  if (depth == 0) return leaf(x, 300);\n"
        "  /* operand stack temporaries live across the call */\n"
        "  var c0 = __suspensions();\n"
        "  var r = (local + 1) * 3 + mid(x + 1, depth - 1) * 7 + [local, depth].length\n"
        "        + leaf(local, 50) + (str + mid(x, depth - 1)).length;\n"
        "  if (__suspensions() != c0) __stats.temps++;\n"
        "  return r % 1000003;\n"
        "}\n"
        "function top(n) {\n"
        "  var res = [];\n"
        "  for (var i = 0; i < n; i++) {\n"
        "    var c0 = __suspensions();\n"
        "    res.push(1 + 2 * mid(i, 5) + i);\n"
        "    if (__suspensions() != c0) __stats.deep++;\n"
        "  }\n"
        "  var o = { f(a, b, c) { return a + '/' + b + '/' + c; } };\n"
        "  var s = o.f('x', top2(3), 'y');\n"
        "  return s + ':' + res.reduce((a, b) => (a + b) % 1000000007, 0);\n"
        "}\n"
        "function top2(n) { var s = 0; for (var i = 0; i < n * 20000; i++) s = (s * 7 + i) % 10007; return s; }\n"
        "print(top(150));\n"
        "'done';\n",
        50, { "deep", "leaf", "temps" }, 0,
    },
    {
        "closures",
        "function makeCounter(start, step) {\n"
        "  var count = start;\n"
        "  let hist = [];\n"
        "  const inc = function () { count += step; hist.push(count); return count; };\n"
        "  const get = () => count + ':' + step + ':' + hist.length;\n"
        "  var c0 = __suspensions();\n"
        "  for (var i = 0; i < 20000; i++) {\n"
        "    if (i % 1000 == 0) inc();\n"
        "    step = (step % 7) + 1;\n"
        "  }\n"
        "  if (__suspensions() != c0) __stats.closure++;\n"
        "  inc();\n"
        "  return { inc, get, peek: () => count };\n"
        "}\n"
        "var cs = [];\n"
        "for (var j = 0; j < 10; j++) cs.push(makeCounter(j, j + 1));\n"
        "for (var j = 0; j < 10; j++) { cs[j].inc(); print(cs[j].get(), cs[j].peek()); }\n"
        "function inner(f, n) {\n"
        "  var s = 0;\n"
        "  for (var i = 0; i < n; i++) s = (s + f(i % 3)) % 100000;\n"
        "  return s;\n"
        "}\n"
        "function outer(a) {\n"
        "  var x = a * 10;\n"
        "  function bump(d) { x += d; a += 1; return x; }\n"
        "  var r = inner(bump, 20000);\n"
        "  return r + ':' + x + ':' + a + ':' + bump(0);\n"
        "}\n"
        "for (var j = 0; j < 5; j++) print(outer(j));\n"
        "function letLoop() {\n"
        "  var fns = [];\n"
        "  for (let i = 0; i < 30000; i++) {\n"
        "    let j = i * 2;\n"
        "    if (i % 3000 == 0) fns.push(() => i + j);\n"
        "  }\n"
        "  return fns.map(f => f()).join(',');\n"
        "}\n"
        "print(letLoop());\n"
        "cs.length;\n",
        10, { "closure" }, 0,
    },
    {
        "mapped_arguments",
        "function mapped(a, b) {\n"
        "  var args = arguments;\n"
        "  var c0 = __suspensions();\n"
        "  for (var i = 0; i < 40000; i++) {\n"
        "    if (i % 5000 == 0) { a = a + 1; args[1] = (args[1] * 2) % 1000; }\n"
        "  }\n"
        "  if (__suspensions() != c0) __stats.mapped++;\n"
        "  var get = function () { return a + b; };\n"
        "  return [a, b, args[0], args[1], args.length, args[2], get()].join(',');\n"
        "}\n"
        "function strict(a, b) {\n"
        "  'use strict';\n"
        "  var args = arguments;\n"
        "  for (var i = 0; i < 40000; i++) { if (i % 5000 == 0) a++; }\n"
        "  return [a, b, args[0], args[1], args.length].join(',');\n"
        "}\n"
        "function rest(a, ...r) {\n"
        "  for (var i = 0; i < 40000; i++) { if (i % 5000 == 0) r.push(i); }\n"
        "  return a + ':' + r.join(',');\n"
        "}\n"
        "print(mapped(1, 3, 'extra'));\n"
        "print(mapped(5));\n"
        "print(mapped(7, 8));\n"
        "print(strict(1, 2, 3));\n"
        "print(strict(1));\n"
        "print(rest(1, 2, 3));\n"
        "'ok';\n",
        10, { "mapped" }, 0,
    },
    {
        "try_catch_finally",
        "function thrower(n) {\n"
        "  var c0 = __suspensions();\n"
        "  var s = 0;\n"
        "  for (var i = 0; i < n; i++) s += i;\n"
        "  if (__suspensions() != c0) __stats.throwAfterResume++;\n"
        "  throw new Error('boom ' + s);\n"
        "}\n"
        "function tc() {\n"
        "  var log = [];\n"
        "  for (var k = 0; k < 5; k++) {\n"
        "    try {\n"
        "      try {\n"
        "        log.push('try' + k);\n"
        "        thrower(30000 + k);\n"
        "        log.push('not reached');\n"
        "      } finally {\n"
        "        var c0 = __suspensions();\n"
        "        var t = 0;\n"
        "        for (var q = 0; q < 20000; q++) t += q;\n"
        "        if (__suspensions() != c0) __stats.finally++;\n"
        "        log.push('finally' + k + ':' + t);\n"
        "      }\n"
        "    } catch (e) {\n"
        "      log.push('catch:' + e.message);\n"
        "    }\n"
        "  }\n"
        "  return log.join('|');\n"
        "}\n"
        "print(tc());\n"
        "var iterable = {\n"
        "  [Symbol.iterator]() {\n"
        "    var n = 0;\n"
        "    return {\n"
        "      next() { return { value: n++, done: n > 20 }; },\n"
        "      return() { print('iterator closed at', n); return {}; }\n"
        "    };\n"
        "  }\n"
        "};\n"
        "function forof() {\n"
        "  var s = 0;\n"
        "  for (var v of iterable) {\n"
        "    var c0 = __suspensions();\n"
        "    for (var i = 0; i < 5000; i++) s = (s + v * i) % 10007;\n"
        "    if (__suspensions() != c0) __stats.forof++;\n"
        "    if (v == 15) break;\n"
        "  }\n"
        "  return s;\n"
        "}\n"
        "print(forof());\n"
        "'end';\n",
        10, { "finally", "throwAfterResume", "forof" }, 0,
    },
    {
        "bound_and_spread",
        "function addMul(a, b, c) {\n"
        "  var c0 = __suspensions();\n"
        "  var r = 0;\n"
        "  for (var i = 0; i < c; i++) r = (r + a * b + i) % 99991;\n"
        "  if (__suspensions() != c0) __stats.bound++;\n"
        "  return r + this.base;\n"
        "}\n"
        "var bf = addMul.bind({ base: 1000 }, 3);\n"
        "var bf2 = bf.bind(null, 4);\n"
        "var tot = 0;\n"
        "for (var i = 0; i < 20; i++) tot += bf(5, 4000) + bf2(3000);\n"
        "print('bound', tot);\n"
        "function sp(a, b, c, d) {\n"
        "  var c0 = __suspensions();\n"
        "  var r = 0;\n"
        "  for (var i = 0; i < 6000; i++) r = (r + a + b * c + d * i) % 99991;\n"
        "  if (__suspensions() != c0) __stats.spread++;\n"
        "  return r + arguments.length;\n"
        "}\n"
        "var arr = [1, 2, 3, 4, 5];\n"
        "var o = { m: sp };\n"
        "tot = 0;\n"
        "for (var i = 0; i < 20; i++) tot += sp(...arr) + o.m(i, ...arr) + bf(...arr);\n"
        "print('spread', tot);\n"
        "tot;\n",
        20, { "bound", "spread" }, 0,
    },
    {
        "tail_calls",
        "function countdown(n, acc) {\n"
        "  if (n == 0) return acc;\n"
        "  var w = 0;\n"
        "  for (var i = 0; i < 1500; i++) w += i & n;\n"
        "  return countdown(n - 1, (acc + w) % 1000003);\n"
        "}\n"
        "var obj = {\n"
        "  k: 3,\n"
        "  m(n) {\n"
        "    var z = 0;\n"
        "    for (var i = 0; i < 3000; i++) z += i;\n"
        "    return this.t(n, z);\n"
        "  },\n"
        "  t(n, z) {\n"
        "    var c0 = __suspensions();\n"
        "    var y = 0;\n"
        "    for (var i = 0; i < 3000; i++) y ^= i * n + this.k;\n"
        "    if (__suspensions() != c0) __stats.tail++;\n"
        "    return y + z;\n"
        "  }\n"
        "};\n"
        "print(countdown(100, 1));\n"
        "var s = 0;\n"
        "for (var i = 0; i < 50; i++) s = (s + obj.m(i)) % 1000003;\n"
        "print(s);\n"
        "s;\n",
        20, { "tail" }, 0,
    },
    {
        "constructors",
        "function Pt(x, y) {\n"
        "  var c0 = __suspensions();\n"
        "  this.x = x;\n"
        "  var s = 0;\n"
        "  for (var i = 0; i < 5000; i++) s += i % (y + 1);\n"
        "  this.s = s;\n"
        "  if (__suspensions() != c0) __stats.ctor++;\n"
        "}\n"
        "Pt.prototype.sum = function () { return this.x + this.s; };\n"
        "function Weird(v) {\n"
        "  for (var i = 0; i < 5000; i++);\n"
        "  if (v) return { w: v };\n"
        "  return 42;\n"
        "}\n"
        "class Base {\n"
        "  constructor(a) {\n"
        "    this.a = a;\n"
        "    var t = 0;\n"
        "    for (var i = 0; i < 4000; i++) t += i;\n"
        "    this.t = t;\n"
        "  }\n"
        "}\n"
        "class Derived extends Base {\n"
        "  constructor(a, b) {\n"
        "    var c0 = __suspensions();\n"
        "    var u = 0;\n"
        "    for (var i = 0; i < 3000; i++) u = (u + i) % 97;\n"
        "    super(a + u);\n"
        "    this.b = b;\n"
        "    if (__suspensions() != c0) __stats.derived++;\n"
        "  }\n"
        "}\n"
        "class Derived2 extends Base {}\n"
        "var BP = Pt.bind(null, 7);\n"
        "var args = [1, 2];\n"
        "var acc = [];\n"
        "for (var i = 0; i < 12; i++) {\n"
        "  var p = new Pt(i, 3), w1 = new Weird(i % 2 ? i : 0), d = new Derived(i, i + 1);\n"
        "  var d2 = new Derived2(i), bp = new BP(5), ps = new Pt(...args);\n"
        "  acc.push([p.sum(), p instanceof Pt, JSON.stringify(w1), w1 instanceof Weird,\n"
        "            d.a, d.b, d.t, d instanceof Derived, d2.a, d2 instanceof Derived2,\n"
        "            bp.x, bp instanceof Pt, ps.sum()].join(','));\n"
        "}\n"
        "print(acc.join('\\n'));\n"
        "acc.length;\n",
        20, { "ctor", "derived" }, 0,
    },
    {
        "defer_native_callbacks",
        "var arr = [];\n"
        "for (var i = 0; i < 20; i++) arr.push(i);\n"
        "var total = 0;\n"
        "arr.forEach(function (v) {\n"
        "  __enter();\n"
        "  for (var k = 0; k < 6000; k++) total = (total + v * k) % 1000003;\n"
        "  __leave();\n"
        "});\n"
        "print('forEach', total);\n"
        "var o = { get g() { __enter(); var s = 0; for (var i = 0; i < 30000; i++) s += i; __leave(); return s; } };\n"
        "print('getter', o.g);\n"
        "var vo = { valueOf() { __enter(); var s = 0; for (var i = 0; i < 30000; i++) s ^= i; __leave(); return s; } };\n"
        "print('valueOf', vo + 1);\n"
        "var px = new Proxy({}, { get(t, k) { __enter(); var s = 0; for (var i = 0; i < 30000; i++) s += i & 7; __leave(); return k + s; } });\n"
        "print('proxy', px.foo);\n"
        "function viaCall(a) { __enter(); var s = 0; for (var i = 0; i < 30000; i++) s += a; __leave(); return s; }\n"
        "print('call', viaCall.call(null, 2), viaCall.apply(null, [3]));\n"
        "print('eval', eval('__enter(); var s = 0; for (var i = 0; i < 30000; i++) s += 1; __leave(); s'));\n"
        "var sorted = [5, 3, 9, 1].sort(function (a, b) { __enter(); for (var i = 0; i < 8000; i++); __leave(); return a - b; });\n"
        "print('sort', sorted.join(','));\n"
        "Error.prepareStackTrace = function (e, cs) { __enter(); var s = 0; for (var i = 0; i < 30000; i++) s += i & 3; __leave(); return 'prepared ' + cs.length + ' ' + s; };\n"
        "print('prepare', new Error('x').stack);\n"
        "try { null.x; } catch (e) { print('prepare thrown', e.stack); }\n"
        "Error.prepareStackTrace = undefined;\n"
        "/* the deferred requests are honored here */\n"
        "var c0 = __suspensions();\n"
        "var z = 0;\n"
        "for (var i = 0; i < 100000; i++) z = (z + i) % 7919;\n"
        "if (__suspensions() != c0) __stats.after++;\n"
        "print('after', z);\n"
        "z;\n",
        3, { "after" }, 1,
    },
    {
        "defer_generators_async_jobs",
        "function* g(n) {\n"
        "  __enter();\n"
        "  var s = 0;\n"
        "  for (var i = 0; i < n; i++) s = (s + i) % 9973;\n"
        "  __leave();\n"
        "  yield s;\n"
        "  __enter();\n"
        "  for (var i = 0; i < n; i++) s = (s * 3 + i) % 9973;\n"
        "  __leave();\n"
        "  return s;\n"
        "}\n"
        "var it = g(40000);\n"
        "print('gen', it.next().value, it.next().value);\n"
        "function sum(n) { var s = 0; for (var i = 0; i < n; i++) s += i; return s; }\n"
        "function* g2() { __enter(); yield sum(40000); yield sum(50000); __leave(); }\n"
        "print('gen2', [...g2()].join(','));\n"
        "async function af(n) {\n"
        "  __enter();\n"
        "  var s = sum(n);\n"
        "  __leave();\n"
        "  await null;\n"
        "  __enter();\n"
        "  s -= sum(n);\n"
        "  __leave();\n"
        "  print('async done', s);\n"
        "}\n"
        "af(40000);\n"
        "Promise.resolve(7).then(function (v) {\n"
        "  /* normal function called from a job */\n"
        "  __enter();\n"
        "  var s = sum(40000) + v;\n"
        "  __leave();\n"
        "  print('job', s);\n"
        "});\n"
        "var c0 = __suspensions();\n"
        "var z = 0;\n"
        "for (var i = 0; i < 100000; i++) z = (z + i) % 7919;\n"
        "if (__suspensions() != c0) __stats.after++;\n"
        "print('after', z);\n"
        "z;\n",
        3, { "after" }, 1,
    },
    {
        /* __check() is called by side_eval() while suspended */
        "closure_while_suspended",
        "function run(n) {\n"
        "  var x = 0, i;\n"
        "  __check = function () { return x - 2 * i; };\n"
        "  for (i = 0; i < n; i++) { x = 2 * (i + 1); }\n"
        "  return x;\n"
        "}\n"
        "var __check;\n"
        "print(run(200000));\n"
        "'ok';\n",
        20, { NULL }, 0,
    },
    {
        "forin_with_switch",
        "function f(o) {\n"
        "  var s = '', n = 0;\n"
        "  for (var k in o) {\n"
        "    with ({ k2: k + k }) {\n"
        "      for (var i = 0; i < 2000; i++) {\n"
        "        switch (i % 4) {\n"
        "        case 0: n += k2.length; break;\n"
        "        case 1: n ^= i; break;\n"
        "        default: n = (n * 3) % 10007;\n"
        "        }\n"
        "      }\n"
        "    }\n"
        "    s += k;\n"
        "    if (k == 'k77') delete o.k78;\n"
        "  }\n"
        "  return s.length + ':' + n;\n"
        "}\n"
        "var o = {};\n"
        "for (var j = 0; j < 100; j++) o['k' + j] = j;\n"
        "print(f(o));\n"
        "'ok';\n",
        20, { NULL }, 0,
    },
    {
        /* 2 polls per iteration (do-while branch + call): the interrupt
           counter always expires at the same poll. When it is the call,
           the next branch must still honor the request. */
        "call_only_loop",
        "var n = 0;\n"
        "function f() { n++; }\n"
        "do { f(); } while (n < 300000);\n"
        "var m = 0;\n"
        "function g() { m += 2; }\n"
        "do { g(); } while (m < 600000);\n"
        "print(n, m);\n"
        "n + m;\n",
        100, { NULL }, 0,
    },
    {
        "exception_after_resume",
        "function f(n) {\n"
        "  var s = 0;\n"
        "  for (var i = 0; i < n; i++) s += i;\n"
        "  if (s > 0) throw new RangeError('escaped ' + s);\n"
        "}\n"
        "print('before');\n"
        "f(200000);\n"
        "print('not reached');\n",
        10, { NULL }, 0,
    },
    {
        "uncaught_value",
        "function g(n) { var o = { n: 0 }; while (o.n < n) o.n++; throw o.n; }\n"
        "try { g(50000); } finally { print('finally runs after resume'); }\n",
        3, { NULL }, 0,
    },
    {
        /* the body and the dispose methods of 'using' declarations are
           suspendable (plain method calls), not the disposers called by
           DisposableStack.prototype.dispose() */
        "using_dispose",
        "var log = [];\n"
        "function spin(n) { var s = 0; for (var i = 0; i < n; i++) s = (s + i) % 10007; return s; }\n"
        "function res(name, fail) {\n"
        "  return { [Symbol.dispose]() {\n"
        "    var c0 = __suspensions();\n"
        "    var s = spin(20000);\n"
        "    if (__suspensions() != c0) __stats.dispose++;\n"
        "    log.push('dispose ' + name + ' ' + s);\n"
        "    if (fail) throw new Error(name);\n"
        "  } };\n"
        "}\n"
        "function body(k) {\n"
        "  using a = res('a' + k), n = null, b = res('b' + k, k == 2);\n"
        "  var c0 = __suspensions();\n"
        "  var s = spin(30000);\n"
        "  if (__suspensions() != c0) __stats.using++;\n"
        "  {\n"
        "    using c = res('c' + k, k == 2);\n"
        "    if (k == 1) return 'early ' + s;\n"
        "    if (k == 2) throw new Error('body');\n"
        "  }\n"
        "  for (using d of [res('d' + k), null, res('e' + k)]) s = (s + spin(5000)) % 10007;\n"
        "  return s;\n"
        "}\n"
        "for (var k = 0; k < 3; k++) {\n"
        "  try { log.push(body(k)); }\n"
        "  catch (e) { log.push(e.name + ':' + e.error.message + ':' + e.suppressed.error.message + ':' + e.suppressed.suppressed.message); }\n"
        "}\n"
        "var st = new DisposableStack();\n"
        "st.defer(function () { __enter(); log.push('defer ' + spin(30000)); __leave(); });\n"
        "st.use({ [Symbol.dispose]() { __enter(); log.push('use ' + spin(30000)); __leave(); } });\n"
        "st.dispose();\n"
        "print(log.join('\\n'));\n"
        "log.length;\n",
        10, { "using", "dispose" }, 1,
    },
};

/* host entered calls: JS_Call() and JS_CallConstructor() */

static const char host_calls_src[] =
    "function work(a, b) {\n"
    "  var s = 0;\n"
    "  for (var i = 0; i < 100000; i++) s = (s + a * i + b) % 1000003;\n"
    "  print('work', a, b, s);\n"
    "  return s;\n"
    "}\n"
    "function Ctor(n) {\n"
    "  this.n = n;\n"
    "  for (var i = 0; i < 50000; i++) this.n = (this.n + i) % 1009;\n"
    "  return 5;\n"
    "}\n"
    "function Ctor2(n) { var s = 0; for (var i = 0; i < 50000; i++) s += n; return { m: s }; }\n"
    "var K = class extends Ctor {\n"
    "  constructor(n) { var t = 0; for (var i = 0; i < 50000; i++) t = (t + i) % 89; super(n + t); this.k = t; }\n"
    "};\n";

static JSValue call_global(JSContext *ctx, const char *name, int ctor,
                           int argc, JSValueConst *argv)
{
    JSValue global, func, ret;
    global = JS_GetGlobalObject(ctx);
    func = JS_GetPropertyStr(ctx, global, name);
    if (ctor)
        ret = JS_CallConstructor(ctx, func, argc, argv);
    else
        ret = JS_Call(ctx, func, global, argc, argv);
    JS_FreeValue(ctx, func);
    JS_FreeValue(ctx, global);
    return ret;
}

static JSValue run_host_calls(JSContext *ctx, const TestCase *tc)
{
    JSValue ret, args[2];
    char *str;

    ret = JS_Eval(ctx, host_calls_src, strlen(host_calls_src), "<host>",
                  JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, finish(ctx, cur, ret));

    args[0] = JS_NewInt32(ctx, 3);
    args[1] = JS_NewInt32(ctx, 4);
    ret = finish(ctx, cur, call_global(ctx, "work", 0, 2, args));
    str = describe(ctx, ret);
    out_str(cur, str); out_str(cur, "\n"); free(str);

    ret = finish(ctx, cur, call_global(ctx, "Ctor", 1, 1, args));
    str = describe(ctx, ret);
    out_str(cur, str); out_str(cur, "\n"); free(str);

    ret = finish(ctx, cur, call_global(ctx, "Ctor2", 1, 1, args));
    str = describe(ctx, ret);
    out_str(cur, str); out_str(cur, "\n"); free(str);

    return call_global(ctx, "K", 1, 1, args);
}

static const TestCase host_calls_test = {
    "host_calls", NULL, 20, { NULL }, 0,
};

/* JS code run by native code reached from host API calls: the native
   continuation forbids the suspension (the requests are deferred to
   the last host call) */

static const char host_native_src[] =
    "function spin(n) { var s = 0; for (var i = 0; i < n; i++) s = (s * 3 + i) % 9973; return s; }\n"
    "var resolveP;\n"
    "var p = new Promise(function (r) { resolveP = r; });\n"
    "p.then(function (v) { print('promise resolved', v); });\n"
    "var thenable = {\n"
    "  get then() {\n"
    "    __enter(); var s = spin(60000); __leave();\n"
    "    return function (r) { __enter(); var t = spin(30000); __leave(); r(s + t); };\n"
    "  }\n"
    "};\n"
    "var og = { get g() { __enter(); var s = spin(60000); __leave(); return s; } };\n"
    "var vo = { toString() { __enter(); var s = spin(60000); __leave(); return 'vo' + s; } };\n"
    "var PX = new Proxy(function () {}, {\n"
    "  construct(t, args) {\n"
    "    __enter(); var s = spin(60000); __leave();\n"
    "    return args[0] ? { v: args[0] + s } : 5;\n"
    "  }\n"
    "});\n"
    "var bound = function (a, b) { __enter(); var s = spin(60000); __leave(); return a + b + s; }.bind(null, 40);\n"
    "var replacer = function (k, v) { if (k === 'x') { __enter(); v += spin(30000); __leave(); } return v; };\n"
    "var data = { x: 1, y: [1, 2] };\n"
    "function after() {\n"
    "  var c0 = __suspensions();\n"
    "  var z = spin(200000);\n"
    "  if (__suspensions() != c0) __stats.after++;\n"
    "  print('after', z);\n"
    "  return z;\n"
    "}\n";

static void out_result(JSContext *ctx, const char *what, JSValue val)
{
    char *str = describe(ctx, finish(ctx, cur, val));
    out_str(cur, what);
    out_str(cur, ": ");
    out_str(cur, str);
    out_str(cur, "\n");
    free(str);
}

static JSValue run_host_native(JSContext *ctx, const TestCase *tc)
{
    JSValue global, f, obj, args[1], ret;

    ret = JS_Eval(ctx, host_native_src, strlen(host_native_src),
                  "<host_native>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, finish(ctx, cur, ret));
    global = JS_GetGlobalObject(ctx);

    /* promise resolving function (class call handler) with a thenable:
       the 'then' getter is run by js_promise_resolve_function_call() */
    f = JS_GetPropertyStr(ctx, global, "resolveP");
    obj = JS_GetPropertyStr(ctx, global, "thenable");
    out_result(ctx, "resolve(thenable)", JS_Call(ctx, f, JS_UNDEFINED, 1, &obj));
    JS_FreeValue(ctx, obj);
    JS_FreeValue(ctx, f);

    /* getter and conversion run by host API calls */
    obj = JS_GetPropertyStr(ctx, global, "og");
    out_result(ctx, "JS_GetProperty(getter)", JS_GetPropertyStr(ctx, obj, "g"));
    JS_FreeValue(ctx, obj);
    obj = JS_GetPropertyStr(ctx, global, "vo");
    out_result(ctx, "JS_ToString", JS_ToString(ctx, obj));
    JS_FreeValue(ctx, obj);

    /* Proxy construct trap: the result is validated after the trap */
    f = JS_GetPropertyStr(ctx, global, "PX");
    args[0] = JS_NewInt32(ctx, 2);
    out_result(ctx, "new PX(2)", JS_CallConstructor(ctx, f, 1, args));
    args[0] = JS_NewInt32(ctx, 0);
    out_result(ctx, "new PX(0)", JS_CallConstructor(ctx, f, 1, args));
    JS_FreeValue(ctx, f);

    /* bound function called by the host */
    f = JS_GetPropertyStr(ctx, global, "bound");
    args[0] = JS_NewInt32(ctx, 2);
    out_result(ctx, "bound(2)", JS_Call(ctx, f, JS_UNDEFINED, 1, args));
    JS_FreeValue(ctx, f);

    /* replacer called by a host API without any frame */
    obj = JS_GetPropertyStr(ctx, global, "data");
    f = JS_GetPropertyStr(ctx, global, "replacer");
    out_result(ctx, "JS_JSONStringify", JS_JSONStringify(ctx, obj, f, JS_UNDEFINED));
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, obj);

    /* the deferred requests are honored by a suspendable host call */
    f = JS_GetPropertyStr(ctx, global, "after");
    ret = JS_Call(ctx, f, global, 0, NULL);
    JS_FreeValue(ctx, f);
    JS_FreeValue(ctx, global);
    return ret;
}

static const TestCase host_native_test = {
    "host_native_paths", NULL, 3, { "after" }, 1,
};

/* JS_FreeRuntime() while suspended */

static const char free_src[] =
    "var keep = [];\n"
    "function deep(x) {\n"
    "  var o = { x: x };\n"
    "  keep.push(function () { return o.x; });\n"
    "  while (true) o.x++;\n"
    "}\n"
    "function holder(a, b) {\n"
    "  var local = { a: a, self: null };\n"
    "  local.self = local;\n"
    "  var f = function () { return local.a + b + arguments.length; };\n"
    "  local.fn = f;\n"
    "  keep.push(f);\n"
    "  var g = (function* () { var q = 0; while (true) { q++; yield q; } })();\n"
    "  g.next();\n"
    "  keep.push(g);\n"
    "  var args = arguments;\n"
    "  keep.push(args);\n"
    "  var s = 0;\n"
    "  for (var i = 0; i < 25000; i++) s += i;\n"
    "  return 'r' + s + [1, 2].length + new Deep(s);\n"
    "}\n"
    "function Deep(s) { this.s = s; return deep.bind(null)(s); }\n"
    "holder(1, 2, 3);\n";

static void test_free_while_suspended(int n_resumes, int gc)
{
    JSRuntime *rt;
    JSContext *ctx;
    JSValue ret;
    RunState st;
    int i;

    memset(&st, 0, sizeof(st));
    cur = &st;
    rt = JS_NewRuntime2(&t_mf, NULL);
    ctx = new_context(rt);
    JS_SetInterruptHandler(rt, interrupt_handler, &st);
    ret = JS_Eval(ctx, free_src, strlen(free_src), "<free>",
                  JS_EVAL_TYPE_GLOBAL);
    for(i = 0; i < n_resumes; i++) {
        CHECK(JS_IsException(ret) && JS_IsSuspended(rt), "not suspended");
        ret = JS_Resume(ctx);
    }
    CHECK(JS_IsException(ret) && JS_IsSuspended(rt), "not suspended");
    if (gc)
        JS_RunGC(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    CHECK(live_blocks == 0, "free while suspended (%d resumes): %ld blocks leaked",
          n_resumes, live_blocks);
    live_blocks = 0;
    free(st.out);
    cur = NULL;
    printf("%-28s freed after %d resumes%s\n", "free_while_suspended",
           n_resumes, gc ? " and GC" : "");
}

/* out of memory while saving the frames: the suspension is aborted
   with an uncatchable error. The frames larger than an allocator arena
   are allocated with the host allocator, where the failure is
   injected. */

static int oom_handler(JSRuntime *rt, void *opaque)
{
    fail_armed = TRUE;
    fail_min_size = 4200;
    JS_RequestSuspend(rt);
    return 0;
}

static void buf_printf(char **pp, char *end, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(*pp, end - *pp, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= end - *pp) {
        fprintf(stderr, "source buffer too small\n");
        exit(1);
    }
    *pp += n;
}

static void test_suspend_oom(int nested)
{
    JSRuntime *rt;
    JSContext *ctx;
    JSValue ret, exc;
    RunState st;
    char *src, *p, *end;
    const char *str;
    int i;
    int32_t v;

    memset(&st, 0, sizeof(st));
    cur = &st;
    /* 'big' has a frame larger than an arena */
    src = malloc(16384);
    p = src;
    end = src + 16384;
    buf_printf(&p, end, "function small(n) { var s = 0; for (var i = 0; i < n; i++) s += i; return s; }\n");
    buf_printf(&p, end, "function big(n) {\n  var v0 = n");
    for(i = 1; i < 300; i++)
        buf_printf(&p, end, ", v%d = v%d + 1", i, i - 1);
    buf_printf(&p, end, ";\n");
    if (nested)
        buf_printf(&p, end, "  return v299 + small(n);\n}\n");
    else
        buf_printf(&p, end, "  for (var i = 0; i < n; i++) v0 += i;\n  return v0 + v299;\n}\n");
    buf_printf(&p, end, "try { print(big(1000000)); } catch (e) { print('caught', e); }\n"
               "finally { print('finally'); }\n");

    rt = JS_NewRuntime2(&t_mf, NULL);
    ctx = new_context(rt);
    JS_SetInterruptHandler(rt, oom_handler, &st);
    ret = JS_Eval(ctx, src, strlen(src), "<oom>", JS_EVAL_TYPE_GLOBAL);
    fail_armed = FALSE;
    JS_SetInterruptHandler(rt, NULL, NULL);
    CHECK(JS_IsException(ret) && !JS_IsSuspended(rt),
          "out of memory while suspending must abort the execution");
    exc = JS_GetException(ctx);
    str = JS_ToCString(ctx, exc);
    CHECK(str && strstr(str, "out of memory"), "unexpected exception: %s",
          str ? str : "?");
    JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, exc);
    CHECK(st.out == NULL, "catch/finally must not run: %s", st.out);

    /* the runtime is still usable */
    ret = JS_Eval(ctx, "small(10)", 9, "<oom2>", JS_EVAL_TYPE_GLOBAL);
    v = 0;
    JS_ToInt32(ctx, &v, ret);
    JS_FreeValue(ctx, ret);
    CHECK(v == 45, "small(10) = %d", v);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    CHECK(live_blocks == 0, "suspend oom: %ld blocks leaked", live_blocks);
    live_blocks = 0;
    free(st.out);
    free(src);
    cur = NULL;
    printf("%-28s %s frame: aborted\n", "suspend_out_of_memory",
           nested ? "outer" : "innermost");
}

/* JS_Resume() fails: the saved frames are terminated (their closures keep
   working, 'finally' does not run) and released at once. 'inner' has a
   frame larger than an allocator arena, allocated by the host
   allocator. */

static int interrupt_after_resume;

static int fail_handler(JSRuntime *rt, void *opaque)
{
    RunState *st = opaque;
    st->handler_calls++;
    if (interrupt_after_resume)
        return 1;
    JS_RequestSuspend(rt);
    return 0;
}

typedef enum {
    RESUME_FAIL_STACK_OVERFLOW, /* the outermost frame cannot be entered */
    RESUME_FAIL_INTERRUPT, /* interrupted while running the resumed frames */
} ResumeFailEnum;

static void test_resume_failure(ResumeFailEnum mode)
{
    JSRuntime *rt;
    JSContext *ctx;
    JSValue ret, global, v;
    RunState st;
    char *src, *p, *end;
    const char *str;
    int i, n_resumes, ready;
    long large_before;
    static const char closures_src[] =
        "[keep(), keep(), keepOuter(), keepOuter()].join()";

    memset(&st, 0, sizeof(st));
    cur = &st;
    interrupt_after_resume = FALSE;
    src = malloc(32768);
    p = src;
    end = src + 32768;
    buf_printf(&p, end, "var keep, keepOuter, ready = false;\n");
    buf_printf(&p, end, "function inner(n) {\n  var local = { v: 1 }, v0 = n");
    for(i = 1; i < 400; i++)
        buf_printf(&p, end, ", v%d = v%d + 1", i, i - 1);
    buf_printf(&p, end, ";\n"
               "  keep = function () { return local.v++ + n; };\n"
               "  var s = 0;\n"
               "  for (var i = 0; i < 2000000; i++) { s += i; ready = true; }\n"
               "  return s + v399;\n"
               "}\n"
               "function outer() {\n"
               "  var o = 10;\n"
               "  keepOuter = function () { return o++; };\n"
               "  try {\n"
               "    return inner(5) + 1;\n"
               "  } catch (e) {\n"
               "    print('catch must not run');\n"
               "  } finally {\n"
               "    print('finally must not run');\n"
               "  }\n"
               "}\n"
               "outer();\n");

    rt = JS_NewRuntime2(&t_mf, NULL);
    ctx = new_context(rt);
    JS_SetInterruptHandler(rt, fail_handler, &st);
    global = JS_GetGlobalObject(ctx);

    /* run until 'inner' is suspended in its loop */
    ret = JS_Eval(ctx, src, strlen(src), "<resume_fail>", JS_EVAL_TYPE_GLOBAL);
    n_resumes = 0;
    for(;;) {
        CHECK(JS_IsException(ret) && JS_IsSuspended(rt), "not suspended");
        if (!JS_IsSuspended(rt))
            goto done;
        v = JS_GetPropertyStr(ctx, global, "ready");
        ready = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        if (ready)
            break;
        ret = JS_Resume(ctx);
        n_resumes++;
    }
    large_before = live_large_blocks;

    if (mode == RESUME_FAIL_STACK_OVERFLOW) {
        JS_SetMaxStackSize(rt, 1);
        ret = JS_Resume(ctx);
        JS_SetMaxStackSize(rt, JS_DEFAULT_STACK_SIZE);
    } else {
        interrupt_after_resume = TRUE;
        ret = JS_Resume(ctx);
        interrupt_after_resume = FALSE;
    }
    CHECK(JS_IsException(ret) && !JS_IsSuspended(rt),
          "JS_Resume() must fail");
    v = JS_GetException(ctx);
    str = JS_ToCString(ctx, v);
    CHECK(str && strstr(str, mode == RESUME_FAIL_STACK_OVERFLOW ?
                        "stack overflow" : "interrupted"),
          "unexpected exception: %s", str ? str : "?");
    JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, v);
    /* released without GC: the closures do not keep the frames */
    CHECK(live_large_blocks < large_before,
          "the frame of 'inner' is not released (%ld large blocks, %ld before)",
          live_large_blocks, large_before);

    /* the closures of the terminated frames keep working */
    JS_SetInterruptHandler(rt, NULL, NULL);
    ret = JS_Eval(ctx, closures_src, strlen(closures_src), "<closures>",
                  JS_EVAL_TYPE_GLOBAL);
    str = JS_ToCString(ctx, ret);
    CHECK(str && !strcmp(str, "6,7,10,11"), "closures: %s", str ? str : "?");
    JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, ret);
    CHECK(st.out == NULL, "catch/finally must not run: %s", st.out);
 done:
    JS_FreeValue(ctx, global);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    CHECK(live_blocks == 0, "resume failure: %ld blocks leaked", live_blocks);
    live_blocks = 0;
    live_large_blocks = 0;
    free(st.out);
    free(src);
    cur = NULL;
    printf("%-28s %s after %d resumes: frames terminated\n", "resume_failure",
           mode == RESUME_FAIL_STACK_OVERFLOW ? "stack overflow" : "interrupt",
           n_resumes);
}

/* API misuse */

static void test_resume_without_suspension(void)
{
    JSRuntime *rt;
    JSContext *ctx;
    JSValue ret;
    RunState st;

    memset(&st, 0, sizeof(st));
    cur = &st;
    rt = JS_NewRuntime2(&t_mf, NULL);
    ctx = new_context(rt);
    CHECK(!JS_IsSuspended(rt), "suspended at start");
    ret = JS_Resume(ctx);
    CHECK(JS_IsException(ret), "JS_Resume() without suspension must fail");
    JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    CHECK(live_blocks == 0, "%ld blocks leaked", live_blocks);
    live_blocks = 0;
    free(st.out);
    cur = NULL;
}

int main(int argc, char **argv)
{
    size_t i;

    verbose = (argc > 1 && !strcmp(argv[1], "-v"));
    for(i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
        run_test(&tests[i], run_eval);
    run_test(&host_calls_test, run_host_calls);
    run_test(&host_native_test, run_host_native);
    test_free_while_suspended(0, 0);
    test_free_while_suspended(3, 1);
    test_free_while_suspended(12, 0);
    test_suspend_oom(0);
    test_suspend_oom(1);
    test_resume_failure(RESUME_FAIL_STACK_OVERFLOW);
    test_resume_failure(RESUME_FAIL_INTERRUPT);
    test_resume_without_suspension();

    if (n_failures) {
        printf("%d failure(s)\n", n_failures);
        return 1;
    }
    printf("test_suspend: OK\n");
    return 0;
}
