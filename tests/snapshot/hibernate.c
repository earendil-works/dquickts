/*
 * Cross-process hibernation with incremental snapshots.
 *
 *   hibernate plain  script.js             run the script normally
 *   hibernate start  script.js base state  set up the runtime, save it as
 *                                          the base, then run one slice and
 *                                          save the state
 *   hibernate resume base state            restore the base and the state,
 *                                          run one slice, save the state
 *
 * The base is the runtime once the host natives are installed. The state
 * is a delta of the base. A slice ends at the first suspension of the
 * running code (requested by the interrupt handler) or after one job.
 * The exit status is 2 when the state was saved (more work to do) and 0
 * when the script and all its jobs are done. Running "start" then
 * "resume" until the status is 0 prints the same output as "plain":
 * every slice runs in a new process.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"

static JSValue js_print(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    int i;
    for(i = 0; i < argc; i++) {
        const char *str = JS_ToCString(ctx, argv[i]);
        if (!str)
            return JS_EXCEPTION;
        printf("%s%s", i ? " " : "", str);
        JS_FreeCString(ctx, str);
    }
    printf("\n");
    return JS_UNDEFINED;
}

static const void * const host_natives[] = { (const void *)js_print };

static const JSSnapshotHost host = {
    .natives = host_natives,
    .native_count = 1,
};

static int interrupt_handler(JSRuntime *rt, void *opaque)
{
    JS_RequestSuspend(rt);
    return 0;
}

static void print_exception(JSContext *ctx)
{
    JSValue exc = JS_GetException(ctx);
    const char *str = JS_ToCString(ctx, exc);
    printf("exception: %s\n", str ? str : "?");
    JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, exc);
}

static JSRuntime *new_runtime(int suspend)
{
    JSRuntime *rt = JS_NewRuntime();
    if (suspend)
        JS_SetInterruptHandler(rt, interrupt_handler, NULL);
    return rt;
}

static uint8_t *load_file(const char *filename, size_t *plen)
{
    FILE *f = fopen(filename, "rb");
    uint8_t *buf;
    long len;
    if (!f) {
        perror(filename);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(len + 1);
    if (fread(buf, 1, len, f) != (size_t)len) {
        perror(filename);
        exit(1);
    }
    buf[len] = '\0';
    fclose(f);
    *plen = len;
    return buf;
}

static int write_file(JSContext *ctx, const char *filename, int flags)
{
    size_t size;
    uint8_t *buf = JS_WriteSnapshot(ctx, &size, &host, NULL, 0, flags);
    FILE *f;
    if (!buf) {
        print_exception(ctx);
        return -1;
    }
    f = fopen(filename, "wb");
    if (!f || fwrite(buf, 1, size, f) != size || fclose(f)) {
        perror(filename);
        js_free(ctx, buf);
        return -1;
    }
    js_free(ctx, buf);
    return 0;
}

/* save the state (delta of the base) and free the runtime */
static int save(JSRuntime *rt, JSContext *ctx, const char *state)
{
    if (write_file(ctx, state, JS_SNAPSHOT_DELTA))
        return 1;
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 2;
}

/* the completion value of the script, or its exception */
static void report(JSContext *ctx, JSValue val)
{
    if (JS_IsException(val)) {
        print_exception(ctx);
    } else {
        const char *str = JS_ToCString(ctx, val);
        printf("completion: %s\n", str ? str : "?");
        JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, val);
    }
}

/* run one job (or nothing), then save the state or finish */
static int end_slice(JSRuntime *rt, JSContext *ctx, const char *state, int run_job)
{
    if (run_job) {
        JSContext *ctx1;
        if (JS_ExecutePendingJob(rt, &ctx1) < 0)
            print_exception(ctx1);
    }
    if (JS_IsJobPending(rt))
        return save(rt, ctx, state);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}

int main(int argc, char **argv)
{
    JSRuntime *rt;
    JSContext *ctx;
    JSValue val, global;
    uint8_t *buf;
    size_t len;
    int ret;

    if ((argc == 3 && !strcmp(argv[1], "plain")) ||
        (argc == 5 && !strcmp(argv[1], "start"))) {
        int plain = !strcmp(argv[1], "plain");
        buf = load_file(argv[2], &len);
        rt = new_runtime(!plain);
        ctx = JS_NewContext(rt);
        global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, js_print, "print", 1));
        JS_FreeValue(ctx, global);
        if (!plain && write_file(ctx, argv[3], JS_SNAPSHOT_BASE))
            return 1;
        val = JS_Eval(ctx, (char *)buf, len, argv[2], JS_EVAL_TYPE_GLOBAL);
        free(buf);
        if (plain) {
            JSContext *ctx1;
            report(ctx, val);
            while ((ret = JS_ExecutePendingJob(rt, &ctx1)) != 0) {
                if (ret < 0)
                    print_exception(ctx1);
            }
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt);
            return 0;
        }
        if (JS_IsException(val) && JS_IsSuspended(rt))
            return save(rt, ctx, argv[4]);
        report(ctx, val);
        return end_slice(rt, ctx, argv[4], 0);
    } else if (argc == 4 && !strcmp(argv[1], "resume")) {
        JSSnapshotResult res;
        uint8_t *state;
        size_t state_len;
        char err[256];

        buf = load_file(argv[2], &len);
        state = load_file(argv[3], &state_len);
        rt = new_runtime(1);
        if (JS_ReadSnapshot(rt, buf, len, state, state_len, &host, &res,
                            err, sizeof(err))) {
            fprintf(stderr, "%s: %s\n", argv[3], err);
            JS_FreeRuntime(rt);
            return 1;
        }
        free(buf);
        free(state);
        ctx = res.contexts[0];
        JS_FreeSnapshotResult(rt, &res);
        if (JS_IsSuspended(rt)) {
            val = JS_Resume(ctx);
            if (JS_IsException(val) && JS_IsSuspended(rt))
                return save(rt, ctx, argv[3]);
            report(ctx, val);
            return end_slice(rt, ctx, argv[3], 0);
        }
        return end_slice(rt, ctx, argv[3], 1);
    }
    fprintf(stderr, "usage: hibernate plain script.js\n"
            "       hibernate start script.js base state\n"
            "       hibernate resume base state\n");
    return 1;
}
