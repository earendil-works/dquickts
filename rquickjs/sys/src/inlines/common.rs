// Rust versions of the `static inline` functions and macros of quickjs.h
// which do not depend on the JSValue representation.

pub use ::core::ffi::{c_char, c_int, c_uint, c_void};

pub type JSValueConst = JSValue;

pub const JS_NULL: JSValue = JS_MKVAL(JS_TAG_NULL, 0);
pub const JS_UNDEFINED: JSValue = JS_MKVAL(JS_TAG_UNDEFINED, 0);
pub const JS_FALSE: JSValue = JS_MKVAL(JS_TAG_BOOL, 0);
pub const JS_TRUE: JSValue = JS_MKVAL(JS_TAG_BOOL, 1);
pub const JS_EXCEPTION: JSValue = JS_MKVAL(JS_TAG_EXCEPTION, 0);
pub const JS_UNINITIALIZED: JSValue = JS_MKVAL(JS_TAG_UNINITIALIZED, 0);

#[inline]
pub unsafe fn JS_VALUE_HAS_REF_COUNT(v: JSValue) -> bool {
    JS_VALUE_GET_TAG(v) as c_uint >= JS_TAG_FIRST as c_uint
}

#[inline]
pub fn JS_NewBool(_ctx: *mut JSContext, val: bool) -> JSValue {
    JS_MKVAL(JS_TAG_BOOL, val as i32)
}

#[inline]
pub fn JS_NewInt32(_ctx: *mut JSContext, val: i32) -> JSValue {
    JS_MKVAL(JS_TAG_INT, val)
}

#[inline]
pub fn JS_NewCatchOffset(_ctx: *mut JSContext, val: i32) -> JSValue {
    JS_MKVAL(JS_TAG_CATCH_OFFSET, val)
}

#[inline]
pub fn JS_NewInt64(_ctx: *mut JSContext, val: i64) -> JSValue {
    if val == val as i32 as i64 {
        JS_MKVAL(JS_TAG_INT, val as i32)
    } else {
        __JS_NewFloat64(val as f64)
    }
}

#[inline]
pub fn JS_NewUint32(_ctx: *mut JSContext, val: u32) -> JSValue {
    if val <= i32::MAX as u32 {
        JS_MKVAL(JS_TAG_INT, val as i32)
    } else {
        __JS_NewFloat64(val as f64)
    }
}

/// `JS_NewFloat64()` without the (unused) context argument: an integer
/// value is stored as an int.
#[inline]
pub fn JS_NewFloat64(d: f64) -> JSValue {
    let val = d as i32;
    /* -0 cannot be represented as integer, so we compare the bit
    representation */
    if d.to_bits() == (val as f64).to_bits() {
        JS_MKVAL(JS_TAG_INT, val)
    } else {
        __JS_NewFloat64(d)
    }
}

#[inline]
pub unsafe fn JS_IsNumber(v: JSValue) -> bool {
    let tag = JS_VALUE_GET_TAG(v);
    tag == JS_TAG_INT || JS_TAG_IS_FLOAT64(tag)
}

#[inline]
pub unsafe fn JS_IsInt(v: JSValue) -> bool {
    JS_VALUE_GET_TAG(v) == JS_TAG_INT
}

#[inline]
pub unsafe fn JS_IsBigInt(v: JSValue) -> bool {
    let tag = JS_VALUE_GET_TAG(v);
    tag == JS_TAG_BIG_INT || tag == JS_TAG_SHORT_BIG_INT
}

#[inline]
pub unsafe fn JS_IsBool(v: JSValue) -> bool {
    JS_VALUE_GET_TAG(v) == JS_TAG_BOOL
}

#[inline]
pub unsafe fn JS_IsNull(v: JSValue) -> bool {
    JS_VALUE_GET_TAG(v) == JS_TAG_NULL
}

#[inline]
pub unsafe fn JS_IsUndefined(v: JSValue) -> bool {
    JS_VALUE_GET_TAG(v) == JS_TAG_UNDEFINED
}

#[inline]
pub unsafe fn JS_IsException(v: JSValue) -> bool {
    JS_VALUE_GET_TAG(v) == JS_TAG_EXCEPTION
}

#[inline]
pub unsafe fn JS_IsUninitialized(v: JSValue) -> bool {
    JS_VALUE_GET_TAG(v) == JS_TAG_UNINITIALIZED
}

#[inline]
pub unsafe fn JS_IsString(v: JSValue) -> bool {
    let tag = JS_VALUE_GET_TAG(v);
    tag == JS_TAG_STRING || tag == JS_TAG_STRING_ROPE
}

#[inline]
pub unsafe fn JS_IsSymbol(v: JSValue) -> bool {
    JS_VALUE_GET_TAG(v) == JS_TAG_SYMBOL
}

#[inline]
pub unsafe fn JS_IsObject(v: JSValue) -> bool {
    JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT
}

#[inline]
pub unsafe fn JS_IsModule(v: JSValue) -> bool {
    JS_VALUE_GET_TAG(v) == JS_TAG_MODULE
}

#[inline]
unsafe fn __js_rc(v: JSValue) -> *mut JSRefCountHeader {
    (JS_VALUE_GET_PTR(v) as *mut u32).sub(1) as *mut JSRefCountHeader
}

/// The context reference count is stored in the memory block header right
/// before the context, like the one of the reference counted values.
#[inline]
unsafe fn __js_ctx_rc(ctx: *mut JSContext) -> *mut JSRefCountHeader {
    (ctx as *mut u32).sub(1) as *mut JSRefCountHeader
}

#[inline]
pub unsafe fn JS_DupContext(ctx: *mut JSContext) -> *mut JSContext {
    (*__js_ctx_rc(ctx)).ref_count += 1;
    ctx
}

#[inline]
pub unsafe fn JS_FreeContext(ctx: *mut JSContext) {
    let p = __js_ctx_rc(ctx);
    (*p).ref_count -= 1;
    if (*p).ref_count <= 0 {
        __JS_FreeContext(ctx);
    }
}

#[inline]
pub unsafe fn JS_FreeValue(ctx: *mut JSContext, v: JSValue) {
    if JS_VALUE_HAS_REF_COUNT(v) {
        let p = __js_rc(v);
        (*p).ref_count -= 1;
        if (*p).ref_count <= 0 {
            __JS_FreeValue(ctx, v);
        }
    }
}

#[inline]
pub unsafe fn JS_FreeValueRT(rt: *mut JSRuntime, v: JSValue) {
    if JS_VALUE_HAS_REF_COUNT(v) {
        let p = __js_rc(v);
        (*p).ref_count -= 1;
        if (*p).ref_count <= 0 {
            __JS_FreeValueRT(rt, v);
        }
    }
}

#[inline]
pub unsafe fn JS_DupValue(_ctx: *mut JSContext, v: JSValue) -> JSValue {
    if JS_VALUE_HAS_REF_COUNT(v) {
        (*__js_rc(v)).ref_count += 1;
    }
    v
}

#[inline]
pub unsafe fn JS_DupValueRT(_rt: *mut JSRuntime, v: JSValue) -> JSValue {
    if JS_VALUE_HAS_REF_COUNT(v) {
        (*__js_rc(v)).ref_count += 1;
    }
    v
}

#[inline]
pub unsafe fn JS_ToUint32(ctx: *mut JSContext, pres: *mut u32, val: JSValue) -> c_int {
    JS_ToInt32(ctx, pres as *mut i32, val)
}

#[inline]
pub unsafe fn JS_ToCString(ctx: *mut JSContext, val: JSValue) -> *const c_char {
    JS_ToCStringLen2(ctx, ptr::null_mut(), val, 0)
}

#[inline]
pub unsafe fn JS_ToCStringLen(
    ctx: *mut JSContext,
    plen: *mut usize,
    val: JSValue,
) -> *const c_char {
    JS_ToCStringLen2(ctx, plen as _, val, 0)
}

#[inline]
pub unsafe fn JS_ToCStringUTF16(ctx: *mut JSContext, val: JSValue) -> *const u16 {
    JS_ToCStringLenUTF16(ctx, ptr::null_mut(), val)
}

#[inline]
pub unsafe fn JS_AtomToCString(ctx: *mut JSContext, atom: JSAtom) -> *const c_char {
    JS_AtomToCStringLen(ctx, ptr::null_mut(), atom)
}

#[inline]
pub unsafe fn JS_GetProperty(ctx: *mut JSContext, this_obj: JSValue, prop: JSAtom) -> JSValue {
    JS_GetPropertyInternal(ctx, this_obj, prop, this_obj, 0)
}

#[inline]
pub unsafe fn JS_SetProperty(
    ctx: *mut JSContext,
    this_obj: JSValue,
    prop: JSAtom,
    val: JSValue,
) -> c_int {
    JS_SetPropertyInternal(ctx, this_obj, prop, val, this_obj, JS_PROP_THROW as c_int)
}

#[inline]
pub unsafe fn JS_NewCFunction(
    ctx: *mut JSContext,
    func: JSCFunction,
    name: *const c_char,
    length: c_int,
) -> JSValue {
    JS_NewCFunction2(ctx, func, name, length, JSCFunctionEnum_JS_CFUNC_generic, 0)
}

#[inline]
pub unsafe fn JS_NewCFunctionMagic(
    ctx: *mut JSContext,
    func: JSCFunctionMagic,
    name: *const c_char,
    length: c_int,
    cproto: JSCFunctionEnum,
    magic: c_int,
) -> JSValue {
    let ft = JSCFunctionType {
        generic_magic: func,
    };
    JS_NewCFunction2(ctx, ft.generic, name, length, cproto, magic)
}
