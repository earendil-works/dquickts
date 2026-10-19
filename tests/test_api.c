/*
 * QuickJS C API tests
 */
#include <stdlib.h>
#include <stddef.h>
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
        { "new WeakMap()", JS_IsWeakMap },
        { "new WeakSet()", JS_IsWeakSet },
        { "new DataView(new ArrayBuffer(1))", JS_IsDataView },
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

/* check that an exception which is an instance of 'ctor' is pending
   and clear it */
static void check_exception(JSContext *ctx, const char *ctor)
{
    char buf[64];

    CHECK(JS_HasException(ctx));
    set_global(ctx, "exc", JS_GetException(ctx));
    snprintf(buf, sizeof(buf), "exc instanceof %s", ctor);
    CHECK(eval_bool(ctx, buf));
}

/* check that 'v' (freed) is the string 'expected' */
static void check_string(JSContext *ctx, JSValue v, const char *expected)
{
    const char *s;

    CHECK(JS_IsString(v));
    s = JS_ToCString(ctx, v);
    CHECK(s && !strcmp(s, expected));
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
}

static void test_utf16_strings(JSContext *ctx)
{
    /* unpaired surrogates at both ends of a valid pair */
    static const uint16_t mixed[] = { 'a', 0xe9, 0xdc00, 0xd83d, 0xde00, 0xd800 };
    static const uint16_t latin1[] = { 'o', 0xe9 };
    JSRuntime *rt = JS_GetRuntime(ctx);
    const uint16_t *u;
    const char *s;
    size_t len;
    JSValue v;

    v = JS_NewStringUTF16(ctx, NULL, 0);
    s = JS_ToCStringLen(ctx, &len, v);
    CHECK(s && len == 0);
    JS_FreeCString(ctx, s);
    u = JS_ToCStringLenUTF16(ctx, &len, v);
    CHECK(u && len == 0);
    JS_FreeCStringUTF16(ctx, u);
    JS_FreeValue(ctx, v);

    v = JS_NewStringUTF16(ctx, mixed, countof(mixed));
    CHECK(!JS_IsException(v));
    set_global(ctx, "s16", JS_DupValue(ctx, v));
    CHECK(eval_bool(ctx, "s16 === 'a\\xe9\\udc00\\ud83d\\ude00\\ud800' && "
                    "s16.codePointAt(3) === 0x1f600 && !s16.isWellFormed()"));
    u = JS_ToCStringLenUTF16(ctx, &len, v);
    CHECK(u && len == countof(mixed) && !memcmp(u, mixed, sizeof(mixed)));
    JS_FreeCStringUTF16(ctx, u);
    /* UTF-8 keeps the unpaired surrogates as 3 byte sequences */
    s = JS_ToCStringLen(ctx, &len, v);
    CHECK(s && len == 13 &&
          !memcmp(s, "a\xc3\xa9\xed\xb0\x80\xf0\x9f\x98\x80\xed\xa0\x80", len));
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);

    /* Latin-1 contents, and 8 bit strings widened to UTF-16 */
    v = JS_NewStringUTF16(ctx, latin1, countof(latin1));
    set_global(ctx, "s8", JS_DupValue(ctx, v));
    CHECK(eval_bool(ctx, "s8 === 'o\\xe9'"));
    u = JS_ToCStringUTF16(ctx, v);
    CHECK(u && u[0] == 'o' && u[1] == 0xe9);
    JS_FreeCStringRT_UTF16(rt, u);
    JS_FreeValue(ctx, v);

    /* other values are converted with ToString() */
    u = JS_ToCStringLenUTF16(ctx, &len, JS_NewInt32(ctx, -12));
    CHECK(u && len == 3 && u[0] == '-' && u[1] == '1' && u[2] == '2');
    JS_FreeCStringUTF16(ctx, u);
    v = eval_str(ctx, "var r = 'x'.repeat(300); r + '\\u20ac' + r");
    u = JS_ToCStringLenUTF16(ctx, &len, v);
    CHECK(u && len == 601 && u[299] == 'x' && u[300] == 0x20ac && u[600] == 'x');
    JS_FreeCStringUTF16(ctx, u);
    JS_FreeValue(ctx, v);
    v = eval_str(ctx, "Symbol('x')");
    len = 1;
    CHECK(JS_ToCStringLenUTF16(ctx, &len, v) == NULL && len == 0);
    check_exception(ctx, "TypeError");
    JS_FreeValue(ctx, v);

    /* the length is checked before reading the buffer */
    CHECK(JS_IsException(JS_NewStringUTF16(ctx, NULL, (size_t)1 << 30)));
    check_exception(ctx, "InternalError");
    JS_FreeCStringUTF16(ctx, NULL);
    JS_FreeCStringRT_UTF16(rt, NULL);
}

static JSValue make_type_error(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    return JS_NewTypeError(ctx, "from %s", "C");
}

static void test_new_errors(JSContext *ctx)
{
    static const struct {
        const char *ctor;
        JSValue (*func)(JSContext *ctx, const char *fmt, ...);
    } tab[] = {
        { "Error", JS_NewPlainError },
        { "InternalError", JS_NewInternalError },
        { "RangeError", JS_NewRangeError },
        { "ReferenceError", JS_NewReferenceError },
        { "SyntaxError", JS_NewSyntaxError },
        { "TypeError", JS_NewTypeError },
    };
    char buf[160];
    JSValue e;
    int i;

    for(i = 0; i < countof(tab); i++) {
        e = tab[i].func(ctx, "the %s %d", "needle", i);
        CHECK(JS_IsError(ctx, e));
        CHECK(!JS_HasException(ctx));
        set_global(ctx, "ne", e);
        snprintf(buf, sizeof(buf),
                 "Object.getPrototypeOf(ne) === %s.prototype && "
                 "ne.message === 'the needle %d' && typeof ne.stack === 'string'",
                 tab[i].ctor, i);
        CHECK(eval_bool(ctx, buf));
    }
    /* the backtrace includes the JS callers of the C function */
    set_global(ctx, "make_type_error",
               JS_NewCFunction(ctx, make_type_error, "make_type_error", 0));
    CHECK(eval_bool(ctx, "function outer_caller() { return make_type_error() } "
                    "var te = outer_caller(); te instanceof TypeError && "
                    "te.message === 'from C' && te.stack.includes('outer_caller')"));
    CHECK(!JS_HasException(ctx));
}

static void test_integrity(JSContext *ctx)
{
    JSValue obj;

    obj = eval_str(ctx, "var so = { a: 1, n: { x: 1 } }; so");
    CHECK(JS_SealObject(ctx, obj) == 1);
    CHECK(eval_bool(ctx, "Object.isSealed(so) && !Object.isFrozen(so)"));
    CHECK(eval_bool(ctx, "so.a = 3; so.b = 4; delete so.a; so.a === 3 && !('b' in so)"));
    JS_FreeValue(ctx, obj);

    obj = eval_str(ctx, "var fo = { a: 1, get g() { return 2 }, n: { x: 1 } }; fo");
    CHECK(JS_FreezeObject(ctx, obj) == 1);
    CHECK(JS_FreezeObject(ctx, obj) == 1);
    CHECK(eval_bool(ctx, "Object.isFrozen(fo) && (fo.a = 3, fo.a === 1)"));
    /* shallow, and accessors stay accessors */
    CHECK(eval_bool(ctx, "fo.n.x = 5; fo.n.x === 5 && !Object.isFrozen(fo.n) && "
                    "Object.getOwnPropertyDescriptor(fo, 'g').get !== undefined"));
    JS_FreeValue(ctx, obj);

    obj = eval_str(ctx, "var fa = [1, 2]; fa");
    CHECK(JS_FreezeObject(ctx, obj) == 1);
    CHECK(eval_bool(ctx, "try { fa.push(3); false } catch (e) { "
                    "e instanceof TypeError && fa.length === 2 }"));
    JS_FreeValue(ctx, obj);

    /* primitives are left unchanged */
    CHECK(JS_SealObject(ctx, JS_NewInt32(ctx, 1)) == 1);
    CHECK(JS_FreezeObject(ctx, JS_UNDEFINED) == 1);

    obj = eval_str(ctx, "new Proxy({}, { preventExtensions() { return false } })");
    CHECK(JS_FreezeObject(ctx, obj) == -1);
    check_exception(ctx, "TypeError");
    JS_FreeValue(ctx, obj);
    obj = eval_str(ctx, "new Proxy({ a: 1 }, { defineProperty() { throw new RangeError('no') } })");
    CHECK(JS_SealObject(ctx, obj) == -1);
    check_exception(ctx, "RangeError");
    JS_FreeValue(ctx, obj);
}

static void test_class_names(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSClassDef def = { "ApiTestClass" };
    JSClassID class_id = 0;
    const char *s;
    JSAtom atom;
    JSValue v;

    v = eval_str(ctx, "new Map()");
    atom = JS_GetClassName(rt, JS_GetClassID(v));
    s = JS_AtomToCString(ctx, atom);
    CHECK(s && !strcmp(s, "Map"));
    JS_FreeCString(ctx, s);
    JS_FreeAtomRT(rt, atom);
    JS_FreeValue(ctx, v);

    JS_NewClassID(&class_id);
    CHECK(JS_GetClassName(rt, class_id) == JS_ATOM_NULL);
    CHECK(JS_NewClass(rt, class_id, &def) == 0);
    atom = JS_GetClassName(rt, class_id);
    s = JS_AtomToCString(ctx, atom);
    CHECK(s && !strcmp(s, "ApiTestClass"));
    JS_FreeCString(ctx, s);
    JS_FreeAtomRT(rt, atom);
    CHECK(JS_GetClassName(rt, 0) == JS_ATOM_NULL);
    CHECK(JS_GetClassName(rt, 0xffff) == JS_ATOM_NULL);
}

static void test_is_module(JSContext *ctx)
{
    static const char mod_src[] = "export const x = 1;";
    JSValue v;

    v = JS_Eval(ctx, mod_src, strlen(mod_src), "<mod>",
                JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    CHECK(JS_IsModule(v));
    JS_FreeValue(ctx, v);
    v = JS_Eval(ctx, "1", 1, "<script>",
                JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    CHECK(!JS_IsException(v) && !JS_IsModule(v));
    JS_FreeValue(ctx, v);
    CHECK(!JS_IsModule(JS_UNDEFINED));
}

static void test_property_values(JSContext *ctx)
{
    JSValue obj, v;

    obj = eval_str(ctx, "var pv = [10, 20, 30]; pv[2**32 + 1] = 'big'; "
                   "pv[-1] = 'neg'; pv");
    v = JS_GetPropertyInt64(ctx, obj, 1);
    CHECK(JS_VALUE_GET_TAG(v) == JS_TAG_INT && JS_VALUE_GET_INT(v) == 20);
    CHECK(JS_IsUndefined(JS_GetPropertyInt64(ctx, obj, 3)));
    check_string(ctx, JS_GetPropertyInt64(ctx, obj, ((int64_t)1 << 32) + 1), "big");
    check_string(ctx, JS_GetPropertyInt64(ctx, obj, -1), "neg");

    /* the key is freed */
    v = JS_GetPropertyValue(ctx, obj, JS_NewInt32(ctx, 2));
    CHECK(JS_VALUE_GET_INT(v) == 30);
    v = JS_GetPropertyValue(ctx, obj, JS_NewString(ctx, "length"));
    CHECK(JS_VALUE_GET_INT(v) == 3);
    v = JS_GetPropertyValue(ctx, obj, eval_str(ctx, "({ toString() { return '-1' } })"));
    check_string(ctx, v, "neg");
    CHECK(JS_IsUndefined(JS_GetPropertyValue(ctx, obj, JS_NewFloat64(ctx, 1.5))));
    CHECK(JS_IsException(JS_GetPropertyValue(ctx, JS_UNDEFINED, JS_NewString(ctx, "k"))));
    check_exception(ctx, "TypeError");
    CHECK(JS_IsException(JS_GetPropertyValue(ctx, obj, eval_str(ctx, "({ toString() { throw new RangeError() } })"))));
    check_exception(ctx, "RangeError");

    /* the key and the value are freed */
    CHECK(JS_SetPropertyValue(ctx, obj, JS_NewInt32(ctx, 3), JS_NewObject(ctx),
                              JS_PROP_THROW) == 1);
    CHECK(JS_SetPropertyValue(ctx, obj, JS_NewString(ctx, "k"), JS_NewInt32(ctx, 7),
                              JS_PROP_THROW) == 1);
    CHECK(eval_bool(ctx, "pv.length === 4 && typeof pv[3] === 'object' && pv.k === 7"));
    JS_FreeValue(ctx, obj);
    obj = eval_str(ctx, "Object.freeze({ a: 1 })");
    CHECK(JS_SetPropertyValue(ctx, obj, JS_NewString(ctx, "a"), JS_NewObject(ctx), 0) == 0);
    CHECK(!JS_HasException(ctx));
    CHECK(JS_SetPropertyValue(ctx, obj, JS_NewString(ctx, "a"), JS_NewObject(ctx),
                              JS_PROP_THROW) == -1);
    check_exception(ctx, "TypeError");
    JS_FreeValue(ctx, obj);
    CHECK(JS_SetPropertyValue(ctx, JS_UNDEFINED, JS_NewString(ctx, "a"), JS_NewObject(ctx),
                              JS_PROP_THROW) == -1);
    check_exception(ctx, "TypeError");
}

static void test_set_length(JSContext *ctx)
{
    JSValue obj;
    int64_t len;

    obj = eval_str(ctx, "var sl = [1, 2, 3, 4, 5]; sl");
    CHECK(JS_SetLength(ctx, obj, 2) >= 0);
    CHECK(eval_bool(ctx, "sl.join() === '1,2'"));
    CHECK(JS_SetLength(ctx, obj, 4) >= 0);
    CHECK(JS_GetLength(ctx, obj, &len) == 0 && len == 4);
    CHECK(eval_bool(ctx, "!(3 in sl)"));
    CHECK(JS_SetLength(ctx, obj, -1) == -1);
    check_exception(ctx, "RangeError");
    CHECK(JS_SetLength(ctx, obj, (int64_t)1 << 32) == -1);
    check_exception(ctx, "RangeError");
    CHECK(JS_FreezeObject(ctx, obj) == 1);
    CHECK(JS_SetLength(ctx, obj, 1) == -1);
    check_exception(ctx, "TypeError");
    JS_FreeValue(ctx, obj);

    /* array-like objects: plain assignment, no truncation */
    obj = eval_str(ctx, "var al = { length: 3, 0: 'a', 2: 'c' }; al");
    CHECK(JS_SetLength(ctx, obj, 1) >= 0);
    CHECK(eval_bool(ctx, "al.length === 1 && al[2] === 'c'"));
    JS_FreeValue(ctx, obj);
    obj = eval_str(ctx, "var ls = { set length(v) { this.got = v } }; ls");
    CHECK(JS_SetLength(ctx, obj, (int64_t)1 << 40) >= 0);
    CHECK(eval_bool(ctx, "ls.got === 2**40"));
    JS_FreeValue(ctx, obj);

    CHECK(JS_SetLength(ctx, JS_UNDEFINED, 1) == -1);
    check_exception(ctx, "TypeError");
}

static void test_conversions(JSContext *ctx)
{
    JSValue v, obj;
    double d;

    obj = JS_NewString(ctx, "  0x1F ");
    v = JS_ToNumber(ctx, obj);
    JS_FreeValue(ctx, obj);
    CHECK(JS_VALUE_GET_TAG(v) == JS_TAG_INT && JS_VALUE_GET_INT(v) == 31);
    obj = JS_NewString(ctx, "12px");
    v = JS_ToNumber(ctx, obj);
    JS_FreeValue(ctx, obj);
    CHECK(JS_ToFloat64(ctx, &d, v) == 0 && d != d);
    v = JS_ToNumber(ctx, JS_UNDEFINED);
    CHECK(JS_ToFloat64(ctx, &d, v) == 0 && d != d);
    v = JS_ToNumber(ctx, JS_NULL);
    CHECK(JS_VALUE_GET_TAG(v) == JS_TAG_INT && JS_VALUE_GET_INT(v) == 0);
    v = JS_ToNumber(ctx, JS_TRUE);
    CHECK(JS_VALUE_GET_TAG(v) == JS_TAG_INT && JS_VALUE_GET_INT(v) == 1);
    obj = eval_str(ctx, "({ valueOf() { return 2.5 } })");
    v = JS_ToNumber(ctx, obj);
    CHECK(JS_ToFloat64(ctx, &d, v) == 0 && d == 2.5);
    JS_FreeValue(ctx, obj);
    obj = eval_str(ctx, "({ valueOf() { throw new RangeError() } })");
    CHECK(JS_IsException(JS_ToNumber(ctx, obj)));
    check_exception(ctx, "RangeError");
    JS_FreeValue(ctx, obj);
    obj = eval_str(ctx, "1n");
    CHECK(JS_IsException(JS_ToNumber(ctx, obj)));
    check_exception(ctx, "TypeError");
    JS_FreeValue(ctx, obj);

    /* objects which cannot be converted with ToString() */
    obj = eval_str(ctx, "Object.create(null)");
    CHECK(JS_IsException(JS_ToString(ctx, obj)));
    check_exception(ctx, "TypeError");
    check_string(ctx, JS_ToObjectString(ctx, obj), "[object Object]");
    JS_FreeValue(ctx, obj);
    obj = eval_str(ctx, "({ [Symbol.toStringTag]: 'Thing', toString() { throw 1 } })");
    check_string(ctx, JS_ToObjectString(ctx, obj), "[object Thing]");
    JS_FreeValue(ctx, obj);
    obj = eval_str(ctx, "[]");
    check_string(ctx, JS_ToObjectString(ctx, obj), "[object Array]");
    JS_FreeValue(ctx, obj);
    check_string(ctx, JS_ToObjectString(ctx, JS_NULL), "[object Null]");
    check_string(ctx, JS_ToObjectString(ctx, JS_UNDEFINED), "[object Undefined]");
    check_string(ctx, JS_ToObjectString(ctx, JS_NewInt32(ctx, 1)), "[object Number]");
    obj = eval_str(ctx, "({ get [Symbol.toStringTag]() { throw new RangeError() } })");
    CHECK(JS_IsException(JS_ToObjectString(ctx, obj)));
    check_exception(ctx, "RangeError");
    JS_FreeValue(ctx, obj);
    obj = eval_str(ctx, "var rp = Proxy.revocable([], {}); rp.revoke(); rp.proxy");
    CHECK(JS_IsException(JS_ToObjectString(ctx, obj)));
    check_exception(ctx, "TypeError");
    JS_FreeValue(ctx, obj);
}

static int u8_free_count;

static void u8_free(JSRuntime *rt, void *opaque, void *ptr)
{
    CHECK(opaque == &u8_free_count);
    u8_free_count++;
}

static void test_uint8_arrays(JSContext *ctx)
{
    static const char *bad_src[] = {
        "new Int32Array(4)",
        "new DataView(new ArrayBuffer(4))",
        "new ArrayBuffer(4)",
        "var du = new Uint8Array(4); du.buffer.transfer(); du",
    };
    uint8_t ext[4] = { 1, 2, 3, 4 };
    uint8_t *p, *base;
    size_t size, base_size;
    JSValue v, buf;
    int i;

    v = eval_str(ctx, "new Uint8Array(0)");
    p = JS_GetUint8Array(ctx, &size, v);
    CHECK(p != NULL && size == 0);
    JS_FreeValue(ctx, v);
    v = JS_NewUint8ArrayCopy(ctx, NULL, 0);
    p = JS_GetUint8Array(ctx, &size, v);
    CHECK(p != NULL && size == 0);
    JS_FreeValue(ctx, v);

    /* the copy is independent of the source */
    v = JS_NewUint8ArrayCopy(ctx, ext, sizeof(ext));
    ext[0] = 9;
    p = JS_GetUint8Array(ctx, &size, v);
    CHECK(p != NULL && size == 4 && p[0] == 1 && p[3] == 4);
    JS_FreeValue(ctx, v);

    /* external memory, released with the array */
    u8_free_count = 0;
    v = JS_NewUint8Array(ctx, ext, sizeof(ext), u8_free, &u8_free_count, 0);
    CHECK(JS_GetTypedArrayType(v) == JS_TYPED_ARRAY_UINT8);
    p = JS_GetUint8Array(ctx, &size, v);
    CHECK(p == ext && size == 4);
    CHECK(JS_SetPropertyUint32(ctx, v, 1, JS_NewInt32(ctx, 200)) == 1);
    CHECK(ext[1] == 200);
    JS_FreeValue(ctx, v);
    CHECK(u8_free_count == 1);

    v = JS_NewUint8Array(ctx, ext, sizeof(ext), u8_free, &u8_free_count, 1);
    buf = JS_GetTypedArrayBuffer(ctx, v, NULL, NULL, NULL);
    CHECK(!JS_IsArrayBuffer(buf) && JS_GetArrayBuffer(ctx, &size, buf) == ext);
    JS_FreeValue(ctx, buf);
    JS_FreeValue(ctx, v);
    CHECK(u8_free_count == 2);

    /* views with an offset */
    v = eval_str(ctx, "var u8ab = new ArrayBuffer(8); new Uint8Array(u8ab, 2, 3)");
    buf = JS_GetTypedArrayBuffer(ctx, v, NULL, NULL, NULL);
    base = JS_GetArrayBuffer(ctx, &base_size, buf);
    p = JS_GetUint8Array(ctx, &size, v);
    CHECK(base != NULL && p == base + 2 && size == 3);
    JS_FreeValue(ctx, buf);
    JS_FreeValue(ctx, v);
    v = eval_str(ctx, "var rab = new ArrayBuffer(4, { maxByteLength: 8 }); "
                 "var tu = new Uint8Array(rab, 1); rab.resize(6); tu");
    p = JS_GetUint8Array(ctx, &size, v);
    CHECK(p != NULL && size == 5);
    JS_FreeValue(ctx, v);
    v = eval_str(ctx, "new Uint8ClampedArray([7, 8])");
    p = JS_GetUint8Array(ctx, &size, v);
    CHECK(p != NULL && size == 2 && p[1] == 8);
    JS_FreeValue(ctx, v);

    for(i = 0; i < countof(bad_src); i++) {
        v = eval_str(ctx, bad_src[i]);
        CHECK(!JS_IsException(v));
        size = 1;
        CHECK(JS_GetUint8Array(ctx, &size, v) == NULL && size == 0);
        check_exception(ctx, "TypeError");
        JS_FreeValue(ctx, v);
    }
    size = 1;
    CHECK(JS_GetUint8Array(ctx, &size, JS_UNDEFINED) == NULL && size == 0);
    check_exception(ctx, "TypeError");
}

static void test_free_macros(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSMemoryUsage m0, m1;
    JSValue v[8];
    JSAtom a[3];
    int i;

    JS_ComputeMemoryUsage(rt, &m0);
    for(i = 0; i < 8; i++)
        v[i] = JS_NewObject(ctx);
    a[0] = JS_NewAtom(ctx, "api_free_atoms_0");
    a[1] = JS_NewAtom(ctx, "api_free_atoms_1");
    a[2] = JS_NewAtom(ctx, "api_free_atoms_2");
    JS_ComputeMemoryUsage(rt, &m1);
    CHECK(m1.obj_count == m0.obj_count + 8 && m1.atom_count == m0.atom_count + 3);

    if (m1.obj_count > 0)
        JS_FreeValues(ctx, v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
    JS_FreeValuesRT(rt, v[7]);
    JS_FreeAtoms(ctx, a[0], a[1]);
    JS_FreeAtomsRT(rt, a[2]);
    JS_ComputeMemoryUsage(rt, &m1);
    CHECK(m1.obj_count == m0.obj_count && m1.atom_count == m0.atom_count);
}

static JSValue fl_func(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    return JS_NewInt32(ctx, argc);
}

static JSValue fl_get(JSContext *ctx, JSValueConst this_val)
{
    return JS_NewInt32(ctx, 1);
}

static JSValue fl_get_magic(JSContext *ctx, JSValueConst this_val, int magic)
{
    return JS_GetPropertyStr(ctx, this_val, "stored");
}

static JSValue fl_set_magic(JSContext *ctx, JSValueConst this_val,
                            JSValueConst val, int magic)
{
    JSValue v = JS_NewInt32(ctx, magic);
    if (JS_SetPropertyStr(ctx, this_val, "stored", JS_DupValue(ctx, val)) < 0 ||
        JS_SetPropertyStr(ctx, this_val, "magic", v) < 0)
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry fl_tab[] = {
    JS_CFUNC_DEF2("f", 2, fl_func, JS_PROP_ENUMERABLE),
    JS_CGETSET_DEF2("g", fl_get, NULL, JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE),
    JS_CGETSET_MAGIC_DEF2("m", fl_get_magic, fl_set_magic, 7, JS_PROP_ENUMERABLE),
    JS_PROP_U2D_DEF("nan", UINT64_C(0x7ff8000000000000), 0),
    JS_PROP_U2D_DEF("negzero", UINT64_C(0x8000000000000000), JS_PROP_ENUMERABLE),
};

static void test_function_list_flags(JSContext *ctx)
{
    JSValue obj = JS_NewObject(ctx);

    CHECK(JS_SetPropertyFunctionList(ctx, obj, fl_tab, countof(fl_tab)) == 0);
    set_global(ctx, "fl", obj);
    CHECK(eval_bool(ctx, "Object.keys(fl).join() === 'f,g,m,negzero'"));
    CHECK(eval_bool(ctx, "var d = Object.getOwnPropertyDescriptor(fl, 'f'); "
                    "!d.writable && !d.configurable && fl.f(1, 2, 3) === 3 && fl.f.length === 2"));
    CHECK(eval_bool(ctx, "var d = Object.getOwnPropertyDescriptor(fl, 'g'); "
                    "d.configurable && d.set === undefined && fl.g === 1"));
    CHECK(eval_bool(ctx, "var d = Object.getOwnPropertyDescriptor(fl, 'm'); "
                    "!d.configurable && (fl.m = 'v', fl.m === 'v' && fl.magic === 7)"));
    CHECK(eval_bool(ctx, "Number.isNaN(fl.nan) && Object.is(fl.negzero, -0) && "
                    "!Object.getOwnPropertyDescriptor(fl, 'nan').writable"));
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
    test_utf16_strings(ctx);
    test_new_errors(ctx);
    test_integrity(ctx);
    test_class_names(ctx);
    test_is_module(ctx);
    test_property_values(ctx);
    test_set_length(ctx);
    test_conversions(ctx);
    test_uint8_arrays(ctx);
    test_free_macros(ctx);
    test_function_list_flags(ctx);
    test_promises(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    test_promise_hooks();
    test_with_resolvers_oom();
    test_detect_module();
    printf("test_api: OK\n");
    return 0;
}
