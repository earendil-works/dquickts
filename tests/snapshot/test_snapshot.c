/*
 * Heap snapshot test host.
 *
 * For every script given on the command line, the output of a normal run
 * is compared with the output of a run that hibernates the runtime at
 * every job boundary: write a snapshot, free the runtime, rebuild it from
 * the snapshot in a new runtime, then execute one more job.
 *
 *   test_snapshot [-v] script.js...
 *
 * Files ending in .mjs are evaluated as modules. The module loader and
 * the promise rejection tracker are host configuration: they are set on
 * every runtime, including the ones rebuilt from a snapshot.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"

typedef struct {
    char *buf;
    size_t len, size;
} Output;

static Output out;
static int verbose;

static void out_append(const char *s, size_t len)
{
    if (out.len + len + 1 > out.size) {
        out.size = (out.len + len + 1) * 2;
        out.buf = realloc(out.buf, out.size);
    }
    memcpy(out.buf + out.len, s, len);
    out.len += len;
    out.buf[out.len] = '\0';
}

static JSValue js_print(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv)
{
    int i;
    for(i = 0; i < argc; i++) {
        size_t len;
        const char *str = JS_ToCStringLen(ctx, &len, argv[i]);
        if (!str)
            return JS_EXCEPTION;
        if (i != 0)
            out_append(" ", 1);
        out_append(str, len);
        JS_FreeCString(ctx, str);
    }
    out_append("\n", 1);
    return JS_UNDEFINED;
}

/* C module "host:native" (see module_loader()) */
static JSValue js_host_twice(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    int32_t v;
    if (JS_ToInt32(ctx, &v, argv[0]))
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, v * 2);
}

static int host_module_init(JSContext *ctx, JSModuleDef *m)
{
    JS_SetModulePrivateValue(ctx, m, JS_NewString(ctx, "private"));
    JS_SetModuleExport(ctx, m, "hostValue", JS_NewInt32(ctx, 21));
    return JS_SetModuleExport(ctx, m, "hostTwice",
                              JS_NewCFunction(ctx, js_host_twice, "hostTwice", 1));
}

static const void * const host_natives[] = {
    (const void *)js_print,
    (const void *)js_host_twice,
    (const void *)host_module_init,
};

static const JSSnapshotHost host = {
    .natives = host_natives,
    .native_count = sizeof(host_natives) / sizeof(host_natives[0]),
};

static void print_exception(JSContext *ctx)
{
    JSValue exc = JS_GetException(ctx);
    const char *str = JS_ToCString(ctx, exc);
    char line[1024];
    snprintf(line, sizeof(line), "exception: %s\n", str ? str : "?");
    out_append(line, strlen(line));
    if (str)
        JS_FreeCString(ctx, str);
    if (JS_IsObject(exc)) {
        JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
        if (verbose && JS_IsString(stack)) {
            str = JS_ToCString(ctx, stack);
            fprintf(stderr, "%s\n", str);
            JS_FreeCString(ctx, str);
        }
        JS_FreeValue(ctx, stack);
    }
    JS_FreeValue(ctx, exc);
}

static JSContext *new_context(JSRuntime *rt)
{
    JSContext *ctx = JS_NewContext(rt);
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, js_print, "print", 1));
    JS_FreeValue(ctx, global);
    return ctx;
}

static char *load_file(const char *filename, size_t *plen)
{
    FILE *f = fopen(filename, "rb");
    char *buf;
    long len;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(len + 1);
    if (fread(buf, 1, len, f) != (size_t)len) {
        fclose(f);
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    *plen = len;
    return buf;
}

static int has_suffix(const char *str, const char *suffix)
{
    size_t len = strlen(str), slen = strlen(suffix);
    return len >= slen && !memcmp(str + len - slen, suffix, slen);
}

/* import.meta.url is the module name (deterministic across runs) */
static void set_import_meta(JSContext *ctx, JSValueConst func_val, int is_main)
{
    JSModuleDef *m = JS_VALUE_GET_PTR(func_val);
    JSAtom name = JS_GetModuleName(ctx, m);
    JSValue meta = JS_GetImportMeta(ctx, m);
    JS_DefinePropertyValueStr(ctx, meta, "url", JS_AtomToString(ctx, name), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, meta, "main", JS_NewBool(ctx, is_main), JS_PROP_C_W_E);
    JS_FreeValue(ctx, meta);
    JS_FreeAtom(ctx, name);
}

/* relative specifiers are resolved against the importing module by the
   default normalizer, so module names are paths from the current
   directory */
static JSModuleDef *module_loader(JSContext *ctx, const char *module_name, void *opaque)
{
    JSValue func_val;
    size_t len;
    char *src;

    if (!strcmp(module_name, "host:native")) {
        JSModuleDef *m = JS_NewCModule(ctx, module_name, host_module_init);
        if (!m || JS_AddModuleExport(ctx, m, "hostValue") ||
            JS_AddModuleExport(ctx, m, "hostTwice"))
            return NULL;
        return m;
    }
    src = load_file(module_name, &len);
    if (!src) {
        JS_ThrowReferenceError(ctx, "could not load module '%s'", module_name);
        return NULL;
    }
    func_val = JS_Eval(ctx, src, len, module_name,
                       JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(src);
    if (JS_IsException(func_val))
        return NULL;
    set_import_meta(ctx, func_val, 0);
    /* the module is referenced by the context */
    JS_FreeValue(ctx, func_val);
    return JS_VALUE_GET_PTR(func_val);
}

static void rejection_tracker(JSContext *ctx, JSValueConst promise, JSValueConst reason,
                              JS_BOOL is_handled, void *opaque)
{
    char line[1024];
    if (is_handled) {
        snprintf(line, sizeof(line), "rejection handled\n");
    } else {
        const char *str = JS_ToCString(ctx, reason);
        snprintf(line, sizeof(line), "unhandled rejection: %s\n", str ? str : "?");
        if (str)
            JS_FreeCString(ctx, str);
        else
            JS_FreeValue(ctx, JS_GetException(ctx));
    }
    out_append(line, strlen(line));
}

static JSRuntime *new_runtime(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JS_SetModuleLoaderFunc(rt, NULL, module_loader, NULL);
    JS_SetHostPromiseRejectionTracker(rt, rejection_tracker, NULL);
    return rt;
}

static void eval_script(JSContext *ctx, const char *src, size_t len, const char *filename)
{
    JSValue val;
    if (has_suffix(filename, ".mjs")) {
        /* the result is the evaluation promise: a rejection is reported
           by the rejection tracker */
        val = JS_Eval(ctx, src, len, filename,
                      JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (!JS_IsException(val)) {
            set_import_meta(ctx, val, 1);
            val = JS_EvalFunction(ctx, val);
        }
    } else {
        val = JS_Eval(ctx, src, len, filename, JS_EVAL_TYPE_GLOBAL);
    }
    if (JS_IsException(val))
        print_exception(ctx);
    JS_FreeValue(ctx, val);
}

/* scripts may define onResume(), called once all jobs are done (after the
   last hibernation in hibernating mode) */
static void call_on_resume(JSContext *ctx)
{
    static const char code[] = "typeof onResume === 'function' && onResume()";
    eval_script(ctx, code, sizeof(code) - 1, "<resume>");
}

/* returns 0 if no job was pending */
static int run_one_job(JSRuntime *rt)
{
    JSContext *ctx;
    int ret = JS_ExecutePendingJob(rt, &ctx);
    if (ret < 0)
        print_exception(ctx);
    return ret != 0;
}

static char *run_plain(const char *src, size_t len, const char *filename)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_context(rt);
    out.len = 0;
    out_append("", 0);
    eval_script(ctx, src, len, filename);
    while (run_one_job(rt))
        continue;
    call_on_resume(ctx);
    while (run_one_job(rt))
        continue;
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return strdup(out.buf);
}

/* -d: the hibernations write deltas of the base written once the script
   has run */
static int delta_mode;
static uint8_t *base_buf;
static size_t base_size;

/* write a snapshot and copy it out of the runtime, as a host would
   write it to storage */
static uint8_t *write_snapshot(JSContext *ctx, size_t *psize, int flags)
{
    uint8_t *snap = JS_WriteSnapshot(ctx, psize, &host, NULL, 0, flags), *buf;
    if (!snap) {
        print_exception(ctx);
        return NULL;
    }
    buf = malloc(*psize);
    memcpy(buf, snap, *psize);
    js_free(ctx, snap);
    return buf;
}

/* hibernate: snapshot 'rt' and rebuild it in a new runtime */
static JSRuntime *hibernate(JSRuntime *rt, JSContext **pctx, size_t *psize)
{
    JSContext *ctx = *pctx;
    JSSnapshotResult res;
    char errbuf[256];
    uint8_t *buf;
    size_t size;
    int ret;

    buf = write_snapshot(ctx, &size, base_buf ? JS_SNAPSHOT_DELTA : 0);
    if (!buf)
        return NULL;
    *psize = size;
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    rt = new_runtime();
    if (base_buf)
        ret = JS_ReadSnapshot(rt, base_buf, base_size, buf, size, &host, &res,
                              errbuf, sizeof(errbuf));
    else
        ret = JS_ReadSnapshot(rt, buf, size, NULL, 0, &host, &res,
                              errbuf, sizeof(errbuf));
    if (ret) {
        char line[512];
        snprintf(line, sizeof(line), "snapshot read error: %s\n", errbuf);
        out_append(line, strlen(line));
        JS_FreeRuntime(rt);
        free(buf);
        return NULL;
    }
    free(buf);
    *pctx = res.contexts[0];
    JS_FreeSnapshotResult(rt, &res);
    return rt;
}

static char *run_hibernating(const char *src, size_t len, const char *filename,
                             int *pcount, size_t *pmax_size)
{
    JSRuntime *rt = new_runtime();
    JSContext *ctx = new_context(rt);
    size_t size;

    out.len = 0;
    out_append("", 0);
    *pcount = 0;
    *pmax_size = 0;
    eval_script(ctx, src, len, filename);
    if (delta_mode) {
        base_buf = write_snapshot(ctx, &base_size, JS_SNAPSHOT_BASE);
        if (!base_buf)
            return strdup(out.buf);
    }
    for(;;) {
        rt = hibernate(rt, &ctx, &size);
        if (!rt)
            return strdup(out.buf);
        (*pcount)++;
        if (size > *pmax_size)
            *pmax_size = size;
        if (!run_one_job(rt))
            break;
    }
    call_on_resume(ctx);
    for(;;) {
        rt = hibernate(rt, &ctx, &size);
        if (!rt)
            return strdup(out.buf);
        (*pcount)++;
        if (size > *pmax_size)
            *pmax_size = size;
        if (!run_one_job(rt))
            break;
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return strdup(out.buf);
}

int main(int argc, char **argv)
{
    int i, failed = 0;

    for(i = 1; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-v"))
            verbose = 1;
        else if (!strcmp(argv[i], "-d"))
            delta_mode = 1;
    }
    for(; i < argc; i++) {
        size_t len, max_size;
        int count;
        char *src = load_file(argv[i], &len);
        char *expected, *got;
        if (!src) {
            fprintf(stderr, "%s: cannot read\n", argv[i]);
            failed++;
            continue;
        }
        expected = run_plain(src, len, argv[i]);
        got = run_hibernating(src, len, argv[i], &count, &max_size);
        if (strcmp(expected, got) != 0) {
            printf("FAIL %s\n--- expected:\n%s--- got:\n%s", argv[i], expected, got);
            failed++;
        } else {
            if (delta_mode)
                printf("ok   %s (%d hibernations, base %zu bytes, max delta %zu bytes)\n",
                       argv[i], count, base_size, max_size);
            else
                printf("ok   %s (%d hibernations, max %zu bytes)\n", argv[i], count, max_size);
            if (verbose)
                printf("%s", got);
        }
        free(expected);
        free(got);
        free(src);
        free(base_buf);
        base_buf = NULL;
    }
    free(out.buf);
    return failed != 0;
}
