/*
 * quickjs-libc tests which need the C API
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

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

int main(int argc, char **argv)
{
    test_eval_script_interrupt();
    return 0;
}
