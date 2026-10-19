/*
 * quickjs-libc tests which need the C API
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

#include "../quickjs-libc.h"

static int interrupt_count;

/* host interrupt handler: '*opaque' tells if the JS code must be
   interrupted */
static int host_interrupt_handler(JSRuntime *rt, void *opaque)
{
    interrupt_count++;
    return *(int *)opaque;
}

static void check(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "test_std_api: %s\n", msg);
        exit(1);
    }
}

static JSValue eval_str(JSContext *ctx, const char *str, int flags)
{
    return JS_Eval(ctx, str, strlen(str), "<test>", flags);
}

/* std.evalScript() installs its own interrupt handler (for SIGINT in
   the REPL): the handler of the host must still be called during the
   evaluation, it must be restored afterwards and its interruptions
   must stay uncatchable */
static void test_eval_script_interrupt(void)
{
    JSRuntime *rt;
    JSContext *ctx;
    JSValue val;
    void *opaque;
    int stop = 0;

    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);
    check(rt && ctx, "cannot create the context");
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader,
                            js_module_check_attributes, NULL);
    js_init_module_std(ctx, "std");
    val = eval_str(ctx, "import * as std from 'std'; globalThis.std = std;",
                   JS_EVAL_TYPE_MODULE);
    val = js_std_await(ctx, val);
    check(!JS_IsException(val), "cannot import std");
    JS_FreeValue(ctx, val);

    JS_SetInterruptHandler(rt, host_interrupt_handler, &stop);
    interrupt_count = 0;
    val = eval_str(ctx, "std.evalScript('for (let i = 0; i < 1e6; i++);')",
                   JS_EVAL_TYPE_GLOBAL);
    check(!JS_IsException(val), "evalScript failed");
    JS_FreeValue(ctx, val);
    check(interrupt_count > 0, "host interrupt handler not called");
    check(JS_GetInterruptHandler(rt, &opaque) == host_interrupt_handler &&
          opaque == &stop, "host interrupt handler not restored");

    stop = 1;
    val = eval_str(ctx,
                   "try { std.evalScript('for (;;);') } catch (e) { 'caught' }",
                   JS_EVAL_TYPE_GLOBAL);
    check(JS_IsException(val), "host interruption was caught");
    check(JS_IsUncatchableException(ctx), "host interruption is catchable");
    JS_FreeValue(ctx, JS_GetException(ctx));

    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* a context with the 'std' and 'os' modules as globals */
static JSContext *new_std_context(JSRuntime *rt)
{
    JSContext *ctx;
    JSValue val;

    ctx = JS_NewContext(rt);
    check(ctx != NULL, "cannot create the context");
    js_init_module_std(ctx, "std");
    js_init_module_os(ctx, "os");
    val = eval_str(ctx, "import * as std from 'std'; import * as os from 'os';"
                   "globalThis.std = std; globalThis.os = os;",
                   JS_EVAL_TYPE_MODULE);
    val = js_std_await(ctx, val);
    check(!JS_IsException(val), "cannot import std and os");
    JS_FreeValue(ctx, val);
    return ctx;
}

static void eval_check(JSContext *ctx, const char *str)
{
    JSValue val = eval_str(ctx, str, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(val)) {
        js_std_dump_error(ctx);
        check(0, str);
    }
    JS_FreeValue(ctx, val);
}

/* value of the global expression 'str' as an int32 */
static int eval_int(JSContext *ctx, const char *str)
{
    JSValue val = eval_str(ctx, str, JS_EVAL_TYPE_GLOBAL);
    int v = -1;
    check(!JS_IsException(val) && JS_ToInt32(ctx, &v, val) == 0, str);
    JS_FreeValue(ctx, val);
    return v;
}

/* the pending exception is an error whose message is 'msg' */
static void check_exception(JSContext *ctx, const char *msg)
{
    JSValue exc = JS_GetException(ctx);
    JSValue m = JS_GetPropertyStr(ctx, exc, "message");
    const char *str = JS_ToCString(ctx, m);
    check(str && !strcmp(str, msg), msg);
    JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, m);
    JS_FreeValue(ctx, exc);
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* js_std_loop_once() and js_std_poll_io(): an event loop driven by
   the host */
static void test_loop_once(void)
{
    JSRuntime *rt;
    JSContext *ctx;
    int ret, i;
    int64_t t0;

    rt = JS_NewRuntime();
    check(rt != NULL, "cannot create the runtime");
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader,
                            js_module_check_attributes, NULL);
    ctx = new_std_context(rt);

    check(js_std_loop_once(ctx) == -1, "loop_once: idle");

    /* the jobs are run */
    eval_check(ctx, "globalThis.n = 0; Promise.resolve().then(() => n++);");
    check(js_std_loop_once(ctx) == -1, "loop_once: jobs");
    check(eval_int(ctx, "n") == 1, "loop_once: job not run");

    /* the delay until the next timer, at most one timer per call */
    eval_check(ctx, "os.setTimeout(() => n++, 0); os.setTimeout(() => n++, 0);"
               "os.setTimeout(() => n += 10, 30);");
    check(js_std_loop_once(ctx) == 0, "loop_once: expired timer");
    check(eval_int(ctx, "n") == 2, "loop_once: first timer");
    check(js_std_loop_once(ctx) == 0, "loop_once: second expired timer");
    check(eval_int(ctx, "n") == 3, "loop_once: second timer");
    ret = js_std_loop_once(ctx);
    check(ret > 0 && ret <= 30, "loop_once: delay of the next timer");
    for(i = 0; i < 100 && (ret = js_std_loop_once(ctx)) > 0; i++)
        js_std_poll_io(ctx, ret);
    check(ret == 0 && eval_int(ctx, "n") == 13, "loop_once: delayed timer");
    check(js_std_loop_once(ctx) == -1, "loop_once: idle after the timers");

    /* the jobs queued by a timer are run by the next call */
    eval_check(ctx, "os.setTimeout(() => Promise.resolve().then(() => n++), 0);");
    check(js_std_loop_once(ctx) == 0, "loop_once: job of a timer pending");
    check(eval_int(ctx, "n") == 13, "loop_once: job of a timer run early");
    check(js_std_loop_once(ctx) == -1, "loop_once: job of a timer");
    check(eval_int(ctx, "n") == 14, "loop_once: job of a timer not run");

    /* the exceptions are left to the host */
    eval_check(ctx, "os.setTimeout(() => { throw Error('timer') }, 0);");
    check(js_std_loop_once(ctx) == -2, "loop_once: throwing timer");
    check_exception(ctx, "timer");
    eval_check(ctx, "queueMicrotask(() => { throw Error('job') });");
    check(js_std_loop_once(ctx) == -2, "loop_once: throwing job");
    check_exception(ctx, "job");
    check(js_std_loop_once(ctx) == -1, "loop_once: idle after errors");

    /* poll_io() does not run the timers and returns at once if there
       is nothing to wait for */
    eval_check(ctx, "globalThis.tid = os.setTimeout(() => n++, 0);");
    t0 = now_ms();
    check(js_std_poll_io(ctx, -1) == 0, "poll_io: no handler");
    check(now_ms() - t0 < 1000, "poll_io: waited without handler");
    check(eval_int(ctx, "n") == 14, "poll_io: timer run");
    eval_check(ctx, "os.clearTimeout(tid);");

    /* read handlers */
    eval_check(ctx,
               "globalThis.fds = os.pipe(); globalThis.got = 0;"
               "os.setReadHandler(fds[0], () => {"
               "  var b = new Uint8Array(1);"
               "  os.read(fds[0], b.buffer, 0, 1);"
               "  if (b[0] == 0x21) throw Error('handler');"
               "  got = b[0];"
               "});");
    t0 = now_ms();
    check(js_std_poll_io(ctx, 20) == 0, "poll_io: timeout");
    check(now_ms() - t0 >= 15, "poll_io: timeout not waited");
    check(eval_int(ctx, "got") == 0, "poll_io: handler without data");
    eval_check(ctx, "os.write(fds[1], new Uint8Array([42]).buffer, 0, 1);");
    check(js_std_poll_io(ctx, 1000) == 0, "poll_io: data");
    check(eval_int(ctx, "got") == 42, "poll_io: handler not called");
    eval_check(ctx, "os.write(fds[1], new Uint8Array([0x21]).buffer, 0, 1);");
    check(js_std_poll_io(ctx, 1000) == -2, "poll_io: throwing handler");
    check_exception(ctx, "handler");
    eval_check(ctx, "os.setReadHandler(fds[0], null);"
               "os.close(fds[0]); os.close(fds[1]);");
    check(js_std_loop_once(ctx) == -1, "loop_once: idle at the end");

    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* test_worker_module.js replies "done" to "abort" */
static const char worker_src[] =
    "const w = new os.Worker('./test_worker_module.js');\n"
    "globalThis.done = 0;\n"
    "w.onmessage = (e) => {\n"
    "    if (e.data.type == 'done') {\n"
    "        done = 1;\n"
    "        w.onmessage = null;\n"
    "    }\n"
    "};\n"
    "w.postMessage({ type: 'abort' });\n";

static atomic_int worker_runtime_count;
static JSRuntime *_Atomic worker_runtime;

static JSRuntime *worker_new_runtime(void)
{
    JSRuntime *rt = JS_NewRuntime();
    atomic_fetch_add(&worker_runtime_count, 1);
    worker_runtime = rt;
    return rt;
}

static JSContext *worker_new_context(JSRuntime *rt)
{
    JSContext *ctx;
    /* the runtime is the one of worker_new_runtime() */
    if (rt != worker_runtime)
        return NULL;
    ctx = JS_NewContext(rt);
    if (ctx) {
        js_init_module_std(ctx, "std");
        js_init_module_os(ctx, "os");
    }
    return ctx;
}

/* js_std_set_worker_new_runtime_func(): the runtimes of the workers
   are created by the host */
static void test_worker_new_runtime(void)
{
    JSRuntime *rt;
    JSContext *ctx;
    JSValue val;

    js_std_set_worker_new_runtime_func(worker_new_runtime);
    js_std_set_worker_new_context_func(worker_new_context);
    rt = JS_NewRuntime();
    check(rt != NULL, "cannot create the runtime");
    js_std_init_handlers(rt);
    JS_SetModuleLoaderFunc2(rt, NULL, js_module_loader,
                            js_module_check_attributes, NULL);
    ctx = new_std_context(rt);
    /* the worker module name is relative to the module name */
    val = JS_Eval(ctx, worker_src, strlen(worker_src),
                  "tests/test_std_api.js", JS_EVAL_TYPE_MODULE);
    val = js_std_await(ctx, val);
    if (JS_IsException(val))
        js_std_dump_error(ctx);
    check(!JS_IsException(val), "cannot create the worker");
    JS_FreeValue(ctx, val);
    js_std_loop(ctx);
    check(eval_int(ctx, "done") == 1, "no reply from the worker");
    check(worker_runtime_count == 1, "worker runtime hook not called");

    js_std_free_handlers(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    js_std_set_worker_new_runtime_func(JS_NewRuntime);
}

int main(int argc, char **argv)
{
    test_eval_script_interrupt();
    test_loop_once();
    test_worker_new_runtime();
    return 0;
}
