/*
 * QuickJS C library
 *
 * Copyright (c) 2017-2018 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef QUICKJS_LIBC_H
#define QUICKJS_LIBC_H

#include <stdio.h>
#include <stdlib.h>

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

JSModuleDef *js_init_module_std(JSContext *ctx, const char *module_name);
JSModuleDef *js_init_module_os(JSContext *ctx, const char *module_name);
void js_std_add_helpers(JSContext *ctx, int argc, char **argv);
void js_std_loop(JSContext *ctx);
JSValue js_std_await(JSContext *ctx, JSValue obj);
void js_std_init_handlers(JSRuntime *rt);
void js_std_free_handlers(JSRuntime *rt);
void js_std_dump_error(JSContext *ctx);
uint8_t *js_load_file(JSContext *ctx, size_t *pbuf_len, const char *filename);
int js_module_set_import_meta(JSContext *ctx, JSValueConst func_val,
                              JS_BOOL use_realpath, JS_BOOL is_main);
/* module types selected by the 'type' import attribute */
typedef enum {
    JS_MODULE_TYPE_JS,
    JS_MODULE_TYPE_JSON,
    JS_MODULE_TYPE_JSON5, /* JSON with the JS_PARSE_JSON_EXT extensions */
    JS_MODULE_TYPE_TEXT, /* default export: the file as a string */
    JS_MODULE_TYPE_BYTES, /* default export: the file as an Uint8Array */
} JSModuleTypeEnum;
/* return the JSModuleTypeEnum given by the 'type' import attribute
   (JS_MODULE_TYPE_JS if absent) or -1 with an exception if the type is
   not supported */
int js_module_get_type(JSContext *ctx, JSValueConst attributes);
/* return the default export of a module of a non JS 'type' whose file
   contents are 'buf'. 'buf' must be allocated with js_malloc() and is
   owned by the function, even in case of exception. */
JSValue js_module_new_value(JSContext *ctx, int type, uint8_t *buf,
                            size_t buf_len, const char *module_name);
int js_module_check_attributes(JSContext *ctx, void *opaque, JSValueConst attributes);
/* return JS_EVAL_FLAG_TYPESCRIPT if 'filename' has a TypeScript
   extension ('.ts', '.mts', '.cts'), 0 otherwise, or -1 with a
   SyntaxError for '.tsx' (JSX is not supported) */
int js_std_get_typescript_flag(JSContext *ctx, const char *filename);
/* set the JS_EVAL_FLAG_TS_LEGACY_DECORATORS and
   JS_EVAL_FLAG_TS_DECORATOR_METADATA flags added for the TypeScript
   sources by js_std_get_typescript_flag() and std.evalScript() (the
   other flags are ignored). The workers inherit them. */
void js_std_set_typescript_flags(JSRuntime *rt, int flags);
/* module name normalizer: the default normalization, then, if the file
   does not exist, './x.js', './x.mjs', './x.cjs' and './x' resolve to
   an existing './x.ts', './x.mts', './x.cts' and './x.ts' */
char *js_module_normalize(JSContext *ctx, const char *base_name,
                          const char *name, void *opaque);
/* module loader: compiles the TypeScript files as TypeScript (see
   js_std_get_typescript_flag()) */
JSModuleDef *js_module_loader(JSContext *ctx,
                              const char *module_name, void *opaque,
                              JSValueConst attributes);
/* print() and console.log() of js_std_add_helpers(). Exported so that
   a JSSnapshotHost can list them in its natives (see qjs --drepl). */
JSValue js_std_print(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv);
JSValue js_std_console_log(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv);
void js_std_eval_binary(JSContext *ctx, const uint8_t *buf, size_t buf_len,
                        int flags);
void js_std_eval_binary_json_module(JSContext *ctx,
                                    const uint8_t *buf, size_t buf_len,
                                    const char *module_name);
void js_std_promise_rejection_tracker(JSContext *ctx, JSValueConst promise,
                                      JSValueConst reason,
                                      JS_BOOL is_handled, void *opaque);
void js_std_set_worker_new_context_func(JSContext *(*func)(JSRuntime *rt));

#ifdef __cplusplus
} /* extern "C" { */
#endif

#endif /* QUICKJS_LIBC_H */
