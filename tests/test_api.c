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

static void test_immutable_array_buffer(JSContext *ctx)
{
    JSValue obj, sab;
    int i;

    obj = JS_NewObject(ctx);
    sab = eval_str(ctx, "new SharedArrayBuffer(1)");
    for(i = 0; i < 2; i++) {
        CHECK(JS_IsImmutableArrayBuffer(JS_NULL) == -1);
        CHECK(JS_IsImmutableArrayBuffer(JS_UNDEFINED) == -1);
        CHECK(JS_IsImmutableArrayBuffer(obj) == -1);
        CHECK(JS_IsImmutableArrayBuffer(sab) == -1);
        CHECK(JS_SetImmutableArrayBuffer(JS_NULL, i) == -1);
        CHECK(JS_SetImmutableArrayBuffer(JS_UNDEFINED, i) == -1);
        CHECK(JS_SetImmutableArrayBuffer(obj, i) == -1);
        CHECK(JS_SetImmutableArrayBuffer(sab, i) == -1);
    }
    JS_FreeValue(ctx, sab);
    JS_FreeValue(ctx, obj);

    obj = eval_str(ctx, "globalThis.ab = new ArrayBuffer(1); ab");
    CHECK(JS_IsImmutableArrayBuffer(obj) == 0);
    CHECK(eval_bool(ctx, "var ta = new Uint8Array(ab); ta[0] = 1; ta[0] === 1"));
    CHECK(JS_SetImmutableArrayBuffer(obj, 1) == 0);
    CHECK(JS_IsImmutableArrayBuffer(obj) == 1);
    CHECK(eval_bool(ctx, "ab.immutable && !Reflect.set(ta, 0, 2) && ta[0] === 1"));
    /* an immutable ArrayBuffer cannot be detached */
    JS_DetachArrayBuffer(ctx, obj);
    CHECK(eval_bool(ctx, "!ab.detached && ab.byteLength === 1"));
    CHECK(JS_SetImmutableArrayBuffer(obj, 0) == 0);
    CHECK(JS_IsImmutableArrayBuffer(obj) == 0);
    CHECK(eval_bool(ctx, "!ab.immutable && Reflect.set(ta, 0, 3) && ta[0] === 3"));
    JS_FreeValue(ctx, obj);
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

/* the small blocks come from arenas, so a memory limit alone does not
   make them fail: take every free small block, then give back all but
   those of size 'keep_size' or, if keep_size is 0, the last 'n' ones */
static void *oom_blocks[1 << 16];
static size_t oom_sizes[1 << 16];

static int oom_fill(JSRuntime *rt, size_t keep_size, int n)
{
    int i, j, count = 0, any = 1;
    size_t size;

    while (any && count < (int)countof(oom_blocks) - 64) {
        any = 0;
        for(size = 8; size <= 496; size += 8) {
            void *p = js_malloc_rt(rt, size);
            if (p) {
                oom_sizes[count] = size;
                oom_blocks[count++] = p;
                any = 1;
            }
        }
    }
    if (keep_size) {
        for(i = j = 0; i < count; i++) {
            if (oom_sizes[i] == keep_size)
                oom_blocks[j++] = oom_blocks[i];
            else
                js_free_rt(rt, oom_blocks[i]);
        }
        count = j;
    } else {
        while (n-- > 0 && count > 0)
            js_free_rt(rt, oom_blocks[--count]);
    }
    return count;
}

/* regression test: running out of memory while instantiating a lazy
   (autoinit) property made it undefined for good and could leak the
   value (bellard/quickjs#580, #576); a failed push on a fast array
   incremented its length (bellard/quickjs#576) */
static void test_autoinit_oom(void)
{
    static const char *const names[] = {
        "JSON.parse", "decodeURIComponent", "JSON.stringify"
    };
    JSRuntime *rt;
    JSContext *ctx;
    JSMemoryUsage stats;
    JSValue global, json, o, v;
    char buf[64];
    int which, i, count, failed, n_failed = 0;
    size_t limit;

    for(which = 0; which < 3; which++) {
        for(i = 0; i < 62 + 16; i++) {
            rt = JS_NewRuntime();
            ctx = JS_NewContext(rt);
            global = JS_GetGlobalObject(ctx);
            json = JS_GetPropertyStr(ctx, global, "JSON");
            o = JS_NewObjectProto(ctx, json);
            v = which == 2 ? JS_NewObject(ctx) : JS_UNDEFINED;
            JS_ComputeMemoryUsage(rt, &stats);
            JS_SetMemoryLimit(rt, stats.malloc_size);
            if (i < 62)
                count = oom_fill(rt, 8 + 8 * i, 0);
            else
                count = oom_fill(rt, 0, i - 62);
            if (which == 0) {
                v = JS_GetPropertyStr(ctx, json, "parse");
                failed = JS_IsException(v);
            } else if (which == 1) {
                v = JS_GetPropertyStr(ctx, global, "decodeURIComponent");
                failed = JS_IsException(v);
            } else {
                failed = JS_SetPropertyStr(ctx, o, "stringify", v) < 0;
                v = JS_UNDEFINED;
            }
            JS_FreeValue(ctx, v);
            if (failed)
                JS_FreeValue(ctx, JS_GetException(ctx));
            while (count > 0)
                js_free_rt(rt, oom_blocks[--count]);
            JS_SetMemoryLimit(rt, -1);
            if (failed) {
                n_failed++;
                snprintf(buf, sizeof(buf), "typeof %s === 'function'",
                         names[which]);
                CHECK(eval_bool(ctx, buf));
            }
            JS_FreeValue(ctx, o);
            JS_FreeValue(ctx, json);
            JS_FreeValue(ctx, global);
            JS_FreeContext(ctx);
            JS_FreeRuntime(rt); /* asserts that no object is leaked */
        }
    }
    CHECK(n_failed > 0);

    n_failed = 0;
    for(limit = 0; limit < 2048; limit += 64) {
        rt = JS_NewRuntime();
        ctx = JS_NewContext(rt);
        global = JS_GetGlobalObject(ctx);
        o = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, global, "a", JS_DupValue(ctx, o));
        JS_ComputeMemoryUsage(rt, &stats);
        JS_SetMemoryLimit(rt, stats.malloc_size + limit);
        for(i = 0; i < 100000; i++) {
            if (JS_SetPropertyUint32(ctx, o, i, JS_NewInt32(ctx, i)) < 0)
                break;
        }
        JS_SetMemoryLimit(rt, -1);
        if (i < 100000) {
            n_failed++;
            JS_FreeValue(ctx, JS_GetException(ctx));
            CHECK(eval_bool(ctx, "a.length === a.filter(() => true).length"));
        }
        JS_FreeValue(ctx, o);
        JS_FreeValue(ctx, global);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }
    CHECK(n_failed > 0);
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

static void test_dump_flags(void)
{
    JSRuntime *rt, *rt2;
    uint64_t flags = JS_DUMP_LEAKS | JS_DUMP_GC;

    rt = JS_NewRuntime();
    rt2 = JS_NewRuntime();
    CHECK(JS_GetDumpFlags(rt) == 0);
    JS_SetDumpFlags(rt, flags);
    /* 0 if the dumps are compiled out (no ENABLE_DUMPS) */
    CHECK(JS_GetDumpFlags(rt) == flags || JS_GetDumpFlags(rt) == 0);
    /* the flags are per runtime */
    CHECK(JS_GetDumpFlags(rt2) == 0);
    JS_SetDumpFlags(rt, 0);
    CHECK(JS_GetDumpFlags(rt) == 0);
    JS_FreeRuntime(rt2);
    JS_FreeRuntime(rt);
}

/* evaluate 'str' and return TRUE if its string conversion contains 'sub' */
static int eval_contains(JSContext *ctx, const char *str, const char *sub)
{
    JSValue v = eval_str(ctx, str);
    const char *s;
    int ret;
    CHECK(!JS_IsException(v));
    s = JS_ToCString(ctx, v);
    CHECK(s != NULL);
    ret = strstr(s, sub) != NULL;
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    return ret;
}

/* string conversion of the pending exception contains 'sub' */
static int exception_contains(JSContext *ctx, const char *sub)
{
    JSValue exc = JS_GetException(ctx);
    JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
    const char *s1 = JS_ToCString(ctx, exc), *s2 = JS_ToCString(ctx, stack);
    int ret = (s1 && strstr(s1, sub)) || (s2 && strstr(s2, sub));
    JS_FreeCString(ctx, s1);
    JS_FreeCString(ctx, s2);
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, exc);
    return ret;
}

static JSValue cfunc_data_sum(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv, int magic,
                              JSValue *func_data)
{
    int32_t a, b;
    if (JS_ToInt32(ctx, &a, func_data[0]) || JS_ToInt32(ctx, &b, argv[0]))
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, a + b + magic);
}

static JSValue cfunc_plain(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    return JS_NewInt32(ctx, argc);
}

static int closure_calls, closure_finalized;

static JSValue closure_func(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv, int magic,
                            void *opaque)
{
    int *counter = opaque;
    /* at least 'length' arguments are readable */
    if (argc < 3)
        CHECK(JS_IsUndefined(argv[2]));
    if (JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "from closure");
    closure_calls++;
    return JS_NewInt32(ctx, ++*counter * 100 + magic);
}

static void closure_finalize(void *opaque)
{
    CHECK(*(int *)opaque > 0);
    closure_finalized++;
}

static void test_cfunctions(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JSValue data = JS_NewInt32(ctx, 40), proto, f;
    static int counter1, counter2;

    /* JS_NewCFunctionData2() */
    set_global(ctx, "fd", JS_NewCFunctionData2(ctx, cfunc_data_sum, "fdname",
                                               1, 7, 1, &data));
    set_global(ctx, "fd0", JS_NewCFunctionData2(ctx, cfunc_data_sum, NULL,
                                                3, 0, 1, &data));
    CHECK(eval_bool(ctx, "fd.name === 'fdname' && fd.length === 1 && fd(2) === 49"));
    CHECK(eval_bool(ctx, "fd0.name === '' && fd0.length === 3"));

    /* JS_NewCFunction3(): explicit prototype */
    proto = eval_str(ctx, "({ marker: 1 })");
    CHECK(!JS_IsException(proto));
    f = JS_NewCFunction3(ctx, cfunc_plain, "f3", 2, JS_CFUNC_generic, 0,
                         proto, 0);
    CHECK(!JS_IsException(f));
    set_global(ctx, "proto3", proto);
    set_global(ctx, "f3", f);
    CHECK(eval_bool(ctx, "Object.getPrototypeOf(f3) === proto3 && f3.marker === 1 && "
                    "f3.name === 'f3' && f3.length === 2 && f3(1, 2, 3) === 3"));

    /* JS_NewCClosure(): opaque and magic given to each call */
    set_global(ctx, "c1", JS_NewCClosure(ctx, closure_func, "cclosure",
                                         closure_finalize, 3, 5, &counter1));
    set_global(ctx, "c2", JS_NewCClosure(ctx, closure_func, NULL,
                                         closure_finalize, 3, 6, &counter2));
    CHECK(eval_bool(ctx, "c1.name === 'cclosure' && c1.length === 3 && "
                    "c2.name === '' && typeof c1 === 'function' && "
                    "c1() === 105 && c1() === 205 && c2() === 106"));
    CHECK(closure_calls == 3 && counter1 == 2 && counter2 == 1);
    CHECK(JS_IsException(eval_str(ctx, "c1('throw')")));
    CHECK(exception_contains(ctx, "TypeError: from closure"));
    CHECK(JS_IsException(eval_str(ctx, "c1('throw')")));
    CHECK(exception_contains(ctx, "at cclosure (native)"));
    CHECK(eval_bool(ctx, "c1.call({}, 1, 2, 3) === 305 && "
                    "c1.bind(null)() === 405"));

    /* the opaque finalizer runs when the closure is freed */
    CHECK(eval_bool(ctx, "delete globalThis.c2"));
    JS_RunGC(rt);
    CHECK(closure_finalized == 1);
    JS_FreeContext(ctx);
    CHECK(closure_finalized == 1);
    JS_FreeRuntime(rt);
    CHECK(closure_finalized == 2);
}

static int trigger_polls;

static int count_interrupt(JSRuntime *rt, void *opaque)
{
    trigger_polls++;
    return 0;
}

static JSValue trigger_interrupt(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    JS_TriggerInterruptHandler(ctx);
    return JS_UNDEFINED;
}

/* JS_TriggerInterruptHandler(): the handler is called at the next poll
   instead of after 10000 instructions */
static void test_trigger_interrupt(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    JS_SetInterruptHandler(rt, count_interrupt, NULL);
    set_global(ctx, "trigger", JS_NewCFunction(ctx, trigger_interrupt,
                                               "trigger", 0));
    /* the first poll of a new context may call the handler */
    CHECK(eval_bool(ctx, "for (var i = 0; i < 10; i++); true"));
    trigger_polls = 0;
    CHECK(eval_bool(ctx, "for (var i = 0; i < 10; i++); true"));
    CHECK(trigger_polls == 0);
    CHECK(eval_bool(ctx, "trigger(); for (var i = 0; i < 10; i++); true"));
    CHECK(trigger_polls == 1);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* proxy handler logging every trap call in 'traps' */
static const char proxy_handler_src[] =
    "globalThis.traps = [];"
    "globalThis.handler = {};"
    "for (const t of ['get', 'set', 'has', 'defineProperty', 'deleteProperty',"
    "                 'getOwnPropertyDescriptor', 'ownKeys'])"
    "    handler[t] = (...args) => { traps.push(t); return Reflect[t](...args); };";

static void test_private_symbols(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JSValue pub, priv, priv2, obj, proxy, v, s;
    JSAtom pub_atom, priv_atom, priv2_atom;
    uint8_t *buf;
    size_t len;

    pub = JS_NewSymbol(ctx, "public", 0);
    priv = JS_NewPrivateSymbol(ctx, "private");
    priv2 = JS_NewPrivateSymbol(ctx, NULL);
    CHECK(JS_IsSymbol(pub) && JS_IsSymbol(priv) && JS_IsSymbol(priv2));
    pub_atom = JS_ValueToAtom(ctx, pub);
    priv_atom = JS_ValueToAtom(ctx, priv);
    priv2_atom = JS_ValueToAtom(ctx, priv2);

    obj = eval_str(ctx, "({ str: 1 })");
    CHECK(JS_DefinePropertyValue(ctx, obj, pub_atom, JS_NewInt32(ctx, 2),
                                 JS_PROP_C_W_E) == 1);
    CHECK(JS_DefinePropertyValue(ctx, obj, priv_atom, JS_NewInt32(ctx, 3),
                                 JS_PROP_C_W_E) == 1);
    CHECK(JS_SetProperty(ctx, obj, priv2_atom, JS_NewInt32(ctx, 4)) == 1);
    set_global(ctx, "o", JS_DupValue(ctx, obj));
    set_global(ctx, "pub", JS_DupValue(ctx, pub));

    /* visible to C */
    v = JS_GetProperty(ctx, obj, priv_atom);
    CHECK(JS_VALUE_GET_TAG(v) == JS_TAG_INT && JS_VALUE_GET_INT(v) == 3);
    CHECK(JS_HasProperty(ctx, obj, priv2_atom) == 1);
    /* hidden from JS reflection */
    CHECK(eval_bool(ctx, "Object.getOwnPropertySymbols(o).length === 1 && "
                    "Object.getOwnPropertySymbols(o)[0] === pub"));
    CHECK(eval_bool(ctx, "Reflect.ownKeys(o).length === 2"));
    CHECK(eval_bool(ctx, "Object.getOwnPropertySymbols(Object.assign({}, o)).length === 1"));
    CHECK(eval_bool(ctx, "JSON.stringify(Object.getOwnPropertyDescriptors(o)).length < 200 && "
                    "Object.keys(Object.getOwnPropertyDescriptors(o)).length === 1"));

    /* proxies: never given to the traps, own properties of the proxy */
    v = eval_str(ctx, proxy_handler_src);
    CHECK(!JS_IsException(v));
    JS_FreeValue(ctx, v);
    proxy = eval_str(ctx, "new Proxy(o, handler)");
    CHECK(!JS_IsException(proxy));
    set_global(ctx, "p", JS_DupValue(ctx, proxy));
    v = JS_GetProperty(ctx, proxy, priv_atom);
    CHECK(JS_IsUndefined(v)); /* not forwarded to the target */
    CHECK(JS_SetProperty(ctx, proxy, priv_atom, JS_NewInt32(ctx, 5)) == 1);
    v = JS_GetProperty(ctx, proxy, priv_atom);
    CHECK(JS_VALUE_GET_TAG(v) == JS_TAG_INT && JS_VALUE_GET_INT(v) == 5);
    CHECK(JS_HasProperty(ctx, proxy, priv_atom) == 1);
    CHECK(JS_DeleteProperty(ctx, proxy, priv_atom, 0) == 1);
    CHECK(JS_HasProperty(ctx, proxy, priv_atom) == 0);
    v = JS_GetProperty(ctx, obj, priv_atom);
    CHECK(JS_VALUE_GET_TAG(v) == JS_TAG_INT && JS_VALUE_GET_INT(v) == 3);
    CHECK(eval_bool(ctx, "traps.length === 0"));
    CHECK(eval_bool(ctx, "Reflect.ownKeys(p).length === 2 && traps.join() === 'ownKeys'"));

    /* JS_WriteObject() writes the enumerable string and symbol
       properties but not the private symbols */
    buf = JS_WriteObject(ctx, &len, obj, 0);
    CHECK(buf != NULL);
    v = JS_ReadObject(ctx, buf, len, 0);
    js_free(ctx, buf);
    CHECK(JS_IsObject(v));
    CHECK(JS_HasProperty(ctx, v, priv_atom) == 0);
    set_global(ctx, "copy", v);
    CHECK(eval_bool(ctx, "Reflect.ownKeys(copy).length === 2 && "
                    "Reflect.ownKeys(copy)[0] === 'str' && "
                    "Reflect.ownKeys(copy)[1] !== pub && "
                    "Reflect.ownKeys(copy)[1].description === 'public' && "
                    "copy[Reflect.ownKeys(copy)[1]] === 2"));

    /* a private symbol keeps its description */
    s = JS_GetPropertyStr(ctx, priv, "description");
    CHECK(JS_IsString(s));
    JS_FreeValue(ctx, s);
    s = JS_GetPropertyStr(ctx, priv2, "description");
    CHECK(JS_IsUndefined(s));

    JS_FreeAtom(ctx, pub_atom);
    JS_FreeAtom(ctx, priv_atom);
    JS_FreeAtom(ctx, priv2_atom);
    JS_FreeValue(ctx, proxy);
    JS_FreeValue(ctx, obj);
    JS_FreeValue(ctx, pub);
    JS_FreeValue(ctx, priv);
    JS_FreeValue(ctx, priv2);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static JSValue eval2(JSContext *ctx, const char *str, const char *filename,
                     int line_num, int eval_flags)
{
    JSEvalOptions options = {
        .version = JS_EVAL_OPTIONS_VERSION,
        .eval_flags = eval_flags,
        .filename = filename,
        .line_num = line_num,
    };
    return JS_Eval2(ctx, str, strlen(str), &options);
}

static void test_eval2(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JSEvalOptions options = { .version = JS_EVAL_OPTIONS_VERSION + 1 };
    JSValue v, this_obj;
    int32_t res;

    /* file name and first line number */
    v = eval2(ctx, "1;\nthrow new Error('at line 2')", "file.js", 10,
              JS_EVAL_TYPE_GLOBAL);
    CHECK(JS_IsException(v));
    CHECK(exception_contains(ctx, "file.js:11"));
    v = eval2(ctx, "1;\n\n  (", "syntax.js", 5, JS_EVAL_TYPE_GLOBAL);
    CHECK(JS_IsException(v));
    CHECK(exception_contains(ctx, "syntax.js:7"));
    v = eval2(ctx, "function f() {\n  return new Error().stack;\n}\nf()",
              "fn.js", 100, JS_EVAL_TYPE_GLOBAL);
    CHECK(JS_IsString(v));
    set_global(ctx, "st", v);
    CHECK(eval_bool(ctx, "st.includes('fn.js:101') && st.includes('fn.js:103')"));
    /* defaults: "<unnamed>", line 1 */
    v = JS_Eval2(ctx, "throw new Error()", 17, NULL);
    CHECK(JS_IsException(v));
    CHECK(exception_contains(ctx, "<unnamed>:1"));
    v = eval2(ctx, "new Error().stack", NULL, 0, JS_EVAL_TYPE_GLOBAL);
    set_global(ctx, "st", v);
    CHECK(eval_bool(ctx, "st.includes('<unnamed>:1')"));

    /* our extra eval flags */
    v = eval2(ctx, "let x: number = 41; x + 1", "a.ts", 1,
              JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_TYPESCRIPT);
    CHECK(JS_ToInt32(ctx, &res, v) == 0 && res == 42);
    v = eval2(ctx, "let y: number = 41; y + 1", "a.js", 1, JS_EVAL_TYPE_GLOBAL);
    CHECK(JS_IsException(v));
    CHECK(exception_contains(ctx, "SyntaxError"));
    v = eval2(ctx, "function deco(c: any) { c.tagged = true; }\n"
              "@deco class C {}\nC.tagged", "d.ts", 1,
              JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_TYPESCRIPT |
              JS_EVAL_FLAG_TS_LEGACY_DECORATORS);
    CHECK(JS_IsBool(v) && JS_ToBool(ctx, v));
    v = eval2(ctx, "export const z: number = 1;", "m.ts", 1,
              JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_TYPESCRIPT |
              JS_EVAL_FLAG_COMPILE_ONLY);
    CHECK(JS_VALUE_GET_TAG(v) == JS_TAG_MODULE);
    JS_FreeValue(ctx, v);

    /* JS_EvalThis2() */
    this_obj = eval_str(ctx, "({ answer: 42 })");
    options.version = JS_EVAL_OPTIONS_VERSION;
    options.eval_flags = JS_EVAL_TYPE_GLOBAL;
    v = JS_EvalThis2(ctx, this_obj, "this.answer", 11, &options);
    CHECK(JS_ToInt32(ctx, &res, v) == 0 && res == 42);
    JS_FreeValue(ctx, this_obj);

    /* unknown options version */
    options.version = JS_EVAL_OPTIONS_VERSION + 1;
    v = JS_Eval2(ctx, "1", 1, &options);
    CHECK(JS_IsException(v));
    CHECK(exception_contains(ctx, "JSEvalOptions"));

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static int buf_contains(const uint8_t *buf, size_t len, const char *str)
{
    size_t i, n = strlen(str);
    for(i = 0; i + n <= len; i++) {
        if (!memcmp(buf + i, str, n))
            return 1;
    }
    return 0;
}

static void test_write_strip(void)
{
    static const char src[] =
        "function marker_fn() { return 'marker body'; }\n"
        "function thrower() { throw new Error('x'); }\n"
        "globalThis.marker_fn = marker_fn; globalThis.thrower = thrower;";
    static const int strip_flags[3] = {
        0, JS_WRITE_OBJ_STRIP_SOURCE, JS_WRITE_OBJ_STRIP_DEBUG,
    };
    size_t len[3];
    int i;

    for(i = 0; i < 3; i++) {
        JSRuntime *rt = JS_NewRuntime();
        JSContext *ctx = JS_NewContext(rt);
        JSValue fun;
        uint8_t *buf;
        int has_source, has_filename;

        fun = JS_Eval(ctx, src, strlen(src), "strip_test.js",
                      JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        CHECK(!JS_IsException(fun));
        buf = JS_WriteObject(ctx, &len[i], fun,
                             JS_WRITE_OBJ_BYTECODE | strip_flags[i]);
        CHECK(buf != NULL);
        JS_FreeValue(ctx, fun);
        has_source = buf_contains(buf, len[i], "return 'marker body'");
        has_filename = buf_contains(buf, len[i], "strip_test.js");
        CHECK(has_source == (i == 0));
        CHECK(has_filename == (i != 2));

        /* the stripped bytecode still runs */
        fun = JS_ReadObject(ctx, buf, len[i], JS_READ_OBJ_BYTECODE);
        js_free(ctx, buf);
        CHECK(!JS_IsException(fun));
        fun = JS_EvalFunction(ctx, fun);
        CHECK(!JS_IsException(fun));
        JS_FreeValue(ctx, fun);
        CHECK(eval_bool(ctx, "marker_fn() === 'marker body'"));
        CHECK(eval_contains(ctx, "marker_fn.toString()", "marker body") == (i == 0));
        CHECK(JS_IsException(eval_str(ctx, "thrower()")));
        CHECK(exception_contains(ctx, "strip_test.js:2") == (i != 2));
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }
    CHECK(len[0] > len[1] && len[1] > len[2]);
}

/* reference counted SharedArrayBuffer memory */
typedef struct {
    int64_t ref_count;
    uint8_t buf[];
} TestSab;

static TestSab *test_sab(void *ptr)
{
    return (TestSab *)((uint8_t *)ptr - offsetof(TestSab, buf));
}

static void *t_sab_alloc(void *opaque, size_t size)
{
    TestSab *sab = calloc(1, sizeof(*sab) + size);
    CHECK(sab != NULL);
    sab->ref_count = 1;
    return sab->buf;
}

static void t_sab_free(void *opaque, void *ptr)
{
    if (--test_sab(ptr)->ref_count == 0)
        free(test_sab(ptr));
}

static void t_sab_dup(void *opaque, void *ptr)
{
    test_sab(ptr)->ref_count++;
}

static const JSSharedArrayBufferFunctions t_sab_funcs = {
    t_sab_alloc, t_sab_free, t_sab_dup, NULL,
};

static void test_read_object2(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx;
    JSValue sab, copy;
    uint8_t *buf, **wtab, **rtab;
    size_t len, wlen, rlen;

    JS_SetSharedArrayBufferFunctions(rt, &t_sab_funcs);
    ctx = JS_NewContext(rt);

    sab = eval_str(ctx, "globalThis.sab = new SharedArrayBuffer(16); "
                   "[sab, sab, new Uint8Array(sab)]");
    CHECK(!JS_IsException(sab));
    buf = JS_WriteObject2(ctx, &len, sab,
                          JS_WRITE_OBJ_SAB | JS_WRITE_OBJ_REFERENCE,
                          &wtab, &wlen);
    CHECK(buf != NULL && wlen == 1);
    copy = JS_ReadObject2(ctx, buf, len,
                          JS_READ_OBJ_SAB | JS_READ_OBJ_REFERENCE,
                          &rtab, &rlen);
    CHECK(!JS_IsException(copy));
    /* the memory of each SharedArrayBuffer read */
    CHECK(rlen == 1 && rtab[0] == wtab[0]);
    CHECK(test_sab(rtab[0])->ref_count == 2);
    set_global(ctx, "copy", copy);
    CHECK(eval_bool(ctx, "copy[0] === copy[1] && copy[0] !== sab && "
                    "(new Uint8Array(sab)[3] = 7, copy[2][3] === 7)"));
    js_free(ctx, rtab);
    /* NULL tab: nothing returned */
    copy = JS_ReadObject2(ctx, buf, len,
                          JS_READ_OBJ_SAB | JS_READ_OBJ_REFERENCE, NULL, &rlen);
    CHECK(!JS_IsException(copy) && rlen == 1);
    JS_FreeValue(ctx, copy);
    /* SharedArrayBuffers are rejected without JS_READ_OBJ_SAB */
    copy = JS_ReadObject2(ctx, buf, len, JS_READ_OBJ_REFERENCE, &rtab, &rlen);
    CHECK(JS_IsException(copy));
    JS_FreeValue(ctx, JS_GetException(ctx));
    js_free(ctx, rtab);
    js_free(ctx, wtab);
    js_free(ctx, buf);
    JS_FreeValue(ctx, sab);
    eval_bool(ctx, "delete globalThis.copy");
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* growable SharedArrayBuffer: the memory is allocated upfront and the
   byte length is shared by the clones (upstream bellard/quickjs issue
   546) */
static void test_growable_sab(void)
{
    JSRuntime *rt;
    JSContext *ctx;
    JSValue sab, copy;
    uint8_t *buf, **wtab, **rtab, *data;
    size_t len, wlen, rlen, size;
    int i;

    /* without SharedArrayBuffer allocator */
    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);
    CHECK(eval_bool(ctx, "var s = new SharedArrayBuffer(1, { maxByteLength: 4096 }); "
                    "var a = new Uint8Array(s); s.grow(4096); a.fill(7); "
                    "s.byteLength === 4096 && a.length === 4096 && a[4095] === 7"));
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    rt = JS_NewRuntime();
    JS_SetSharedArrayBufferFunctions(rt, &t_sab_funcs);
    ctx = JS_NewContext(rt);
    sab = eval_str(ctx, "globalThis.sab = new SharedArrayBuffer(1, { maxByteLength: 16 }); "
                   "globalThis.view = new Uint8Array(sab); sab");
    CHECK(!JS_IsException(sab));
    buf = JS_WriteObject2(ctx, &len, sab, JS_WRITE_OBJ_SAB, &wtab, &wlen);
    CHECK(buf != NULL && wlen == 1);
    for(i = 0; i < 2; i++) {
        copy = JS_ReadObject2(ctx, buf, len, JS_READ_OBJ_SAB, &rtab, &rlen);
        CHECK(!JS_IsException(copy));
        CHECK(rlen == 1 && rtab[0] == wtab[0]);
        CHECK(test_sab(rtab[0])->ref_count == 2 + i);
        js_free(ctx, rtab);
        set_global(ctx, i == 0 ? "copy" : "copy2", copy);
    }
    /* the growth is visible from all the clones and their views */
    CHECK(eval_bool(ctx, "copy.grow(8); new Uint8Array(copy)[7] = 5; "
                    "view[7] === 5 && view.length === 8 && sab.byteLength === 8 && "
                    "copy2.byteLength === 8 && copy2.maxByteLength === 16"));
    data = JS_GetArrayBuffer(ctx, &size, sab);
    CHECK(data != NULL && size == 8 && data[7] == 5);
    /* cannot shrink: another clone already grew the memory */
    CHECK(eval_bool(ctx, "var ok; copy2.grow(12); "
                    "try { copy.grow(10); ok = false; } "
                    "catch(e) { ok = e instanceof RangeError; } "
                    "ok && view.length === 12 && copy.byteLength === 12"));
    CHECK(eval_bool(ctx, "sab.grow(12); sab.byteLength === 12"));
    js_free(ctx, wtab);
    js_free(ctx, buf);
    JS_FreeValue(ctx, sab);
    eval_bool(ctx, "delete globalThis.sab; delete globalThis.view; "
              "delete globalThis.copy; delete globalThis.copy2");
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static char finalizer_log[8];

static void runtime_finalizer(JSRuntime *rt, void *arg)
{
    strncat(finalizer_log, arg, sizeof(finalizer_log) - strlen(finalizer_log) - 1);
}

static void test_runtime(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    JS_SetGCThreshold(rt, 123456);
    CHECK(JS_GetGCThreshold(rt) == 123456);
    JS_SetGCThreshold(rt, -1);
    CHECK(JS_GetGCThreshold(rt) == (size_t)-1);

    CHECK(JS_AddRuntimeFinalizer(rt, runtime_finalizer, (void *)"a") == 0);
    CHECK(JS_AddRuntimeFinalizer(rt, runtime_finalizer, (void *)"b") == 0);
    JS_FreeContext(ctx);
    CHECK(finalizer_log[0] == '\0');
    JS_FreeRuntime(rt);
    /* LIFO order, once */
    CHECK(!strcmp(finalizer_log, "ba"));
}

static char normalize_log[256];

static char *normalize2(JSContext *ctx, const char *base_name,
                        const char *name, JSValueConst attributes,
                        void *opaque)
{
    JSValue str = JS_JSONStringify(ctx, attributes, JS_UNDEFINED, JS_UNDEFINED);
    const char *s = JS_IsException(str) ? NULL : JS_ToCString(ctx, str);
    size_t len = strlen(normalize_log);
    CHECK(opaque == normalize_log);
    snprintf(normalize_log + len, sizeof(normalize_log) - len, "%s:%s;",
             name, JS_IsUndefined(str) ? "undefined" : s);
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, str);
    return js_strdup(ctx, name);
}

static int loader_calls;

static JSModuleDef *loader2(JSContext *ctx, const char *name, void *opaque,
                            JSValueConst attributes)
{
    static const char src[] = "export default globalThis.counter = (globalThis.counter ?? 0) + 1;";
    JSValue fun = JS_Eval(ctx, src, strlen(src), name,
                          JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(fun))
        return NULL;
    loader_calls++;
    JS_FreeValue(ctx, fun);
    return JS_VALUE_GET_PTR(fun);
}

static void test_normalize2(void)
{
    static const char src[] =
        "import a from 'dep.js';\n"
        "import b from 'dep.js' with { kind: 'x' };\n"
        "import c from 'dep.js' with { kind: 'x' };\n"
        "globalThis.res = [a, b, c].join();\n"
        "import('dep.js', { with: { kind: 'y' } }).then(ns => globalThis.dyn = ns.default);\n";
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JSValue v;

    JS_SetModuleLoaderFunc2(rt, NULL, loader2, NULL, normalize_log);
    JS_SetModuleNormalizeFunc2(rt, normalize2);
    v = JS_Eval(ctx, src, strlen(src), "main.js", JS_EVAL_TYPE_MODULE);
    CHECK(!JS_IsException(v));
    JS_FreeValue(ctx, v);
    run_jobs(rt);
    CHECK(!strcmp(normalize_log, "dep.js:undefined;dep.js:{\"kind\":\"x\"};"
                  "dep.js:{\"kind\":\"x\"};dep.js:{\"kind\":\"y\"};"));
    /* the modules are keyed by name and attributes */
    CHECK(loader_calls == 3);
    CHECK(eval_bool(ctx, "res === '1,2,2' && dyn === 3"));

    /* JS_SetModuleLoaderFunc2() resets the attribute-aware normalizer */
    JS_SetModuleLoaderFunc2(rt, NULL, loader2, NULL, normalize_log);
    normalize_log[0] = '\0';
    v = JS_Eval(ctx, "import d from 'other.js';", 25, "main2.js",
                JS_EVAL_TYPE_MODULE);
    CHECK(!JS_IsException(v));
    JS_FreeValue(ctx, v);
    run_jobs(rt);
    CHECK(normalize_log[0] == '\0' && loader_calls == 4);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static int calloc_calls, calloc_large_calls;

static void *t_calloc(JSMallocState *s, size_t count, size_t size)
{
    void *ptr = calloc(count, size);
    if (ptr) {
        calloc_calls++;
        if (count * size >= 4096)
            calloc_large_calls++;
        s->malloc_count++;
    }
    return ptr;
}

static void *t_malloc(JSMallocState *s, size_t size)
{
    void *ptr = malloc(size);
    if (ptr)
        s->malloc_count++;
    return ptr;
}

static void t_free(JSMallocState *s, void *ptr)
{
    if (ptr)
        s->malloc_count--;
    free(ptr);
}

static void *t_realloc(JSMallocState *s, void *ptr, size_t size)
{
    if (!ptr)
        return size ? t_malloc(s, size) : NULL;
    if (size == 0) {
        t_free(s, ptr);
        return NULL;
    }
    return realloc(ptr, size);
}

static const JSMallocFunctions t_mf = {
    t_calloc, t_malloc, t_free, t_realloc, NULL,
};

static void test_calloc(void)
{
    JSRuntime *rt = JS_NewRuntime2(&t_mf, NULL);
    JSContext *ctx;
    uint8_t *p;
    int i, n, ok;

    CHECK(rt != NULL);
    CHECK(calloc_calls > 0); /* the runtime itself, class table... */
    ctx = JS_NewContext(rt);
    n = calloc_large_calls;
    /* large zero initialized engine allocations use the host calloc */
    CHECK(eval_bool(ctx, "new Uint8Array(new ArrayBuffer(100000)).every(x => x === 0)"));
    CHECK(calloc_large_calls > n);

    p = js_calloc_rt(rt, 3, 7);
    CHECK(p != NULL);
    for(i = 0, ok = 1; i < 21; i++)
        ok &= p[i] == 0;
    CHECK(ok);
    js_free_rt(rt, p);
    p = js_calloc(ctx, 1000, 10);
    CHECK(p != NULL && p[0] == 0 && p[9999] == 0);
    js_free(ctx, p);
    /* count * size overflow */
    CHECK(js_calloc_rt(rt, SIZE_MAX / 2, 4) == NULL);
    CHECK(js_calloc(ctx, SIZE_MAX / 2, 4) == NULL);
    CHECK(exception_contains(ctx, "out of memory"));
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* bellard/quickjs#581: a WeakMap value is kept only while its key is
   reachable and one GC pass collects the unreachable entries */
static void test_weak_map_ephemeron(void)
{
    static const char src[] =
        "globalThis.map = new WeakMap();\n"
        "(function () {\n"
        "  var key = {}, keys = [], i;\n"
        "  map.set(key, { backRef: key });\n"
        "  globalThis.selfRef = new WeakRef(key);\n"
        "  key = {};\n"
        "  for (i = 0; i < 20; i++) {\n"
        "    var next = {};\n"
        "    map.set(key, { child: next });\n"
        "    key = next;\n"
        "  }\n"
        "  globalThis.chainTail = new WeakRef(key);\n"
        "  /* reachable chain, records in reverse order */\n"
        "  for (i = 0; i <= 20; i++)\n"
        "    keys.push({ id: i });\n"
        "  for (i = 19; i >= 0; i--)\n"
        "    map.set(keys[i], { next: keys[i + 1] });\n"
        "  globalThis.head = keys[0];\n"
        "})();\n";
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    JSValue v;

    v = eval_str(ctx, src);
    CHECK(!JS_IsException(v));
    JS_FreeValue(ctx, v);
    JS_RunGC(rt);
    CHECK(eval_bool(ctx, "selfRef.deref() === undefined"));
    CHECK(eval_bool(ctx, "chainTail.deref() === undefined"));
    CHECK(eval_bool(ctx, "var k = head; for (var i = 0; i < 20; i++) "
                    "k = map.get(k).next; k.id === 20"));
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* the partial result is freed when a BigInt power fails */
static void test_bigint_pow_leak(void)
{
    JSRuntime *rt;
    JSContext *ctx;
    JSMemoryUsage mu1, mu2;
    const char *str = "try { 3n ** 10000000n } catch (e) {}";
    int i;

    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);
    for(i = 0; i < 2; i++) {
        JS_ComputeMemoryUsage(rt, &mu1);
        JS_FreeValue(ctx, eval_str(ctx, str));
        JS_ComputeMemoryUsage(rt, &mu2);
    }
    CHECK(mu2.malloc_count == mu1.malloc_count);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

int main(void)
{
    JSRuntime *rt;
    JSContext *ctx;

    test_version();
    test_dump_flags();
    test_cfunctions();
    test_private_symbols();
    test_eval2();
    test_write_strip();
    test_read_object2();
    test_growable_sab();
    test_runtime();
    test_normalize2();
    test_calloc();
    test_weak_map_ephemeron();
    test_trigger_interrupt();
    test_bigint_pow_leak();

    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);
    test_objects(ctx);
    test_numbers(ctx);
    test_predicates(ctx);
    test_typed_arrays(ctx);
    test_immutable_array_buffer(ctx);
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
    test_autoinit_oom();
    test_detect_module();
    printf("test_api: OK\n");
    return 0;
}
