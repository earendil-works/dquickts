use crate::{qjs, Ctx, Error, Result, StdString, Value};
use alloc::{borrow::Cow, vec::Vec};
use core::{ffi::c_char, mem, ptr::NonNull, slice, str};

/// Borrowed contents of a JavaScript string in its internal representation.
pub(crate) enum StringData<'a> {
    /// One Latin-1 (ISO-8859-1) byte per code unit.
    Latin1(&'a [u8]),
    /// UTF-16 code units, possibly with unpaired surrogates.
    Utf16(&'a [u16]),
}

/// Encode Latin-1 bytes as UTF-8.
fn latin1_to_string(bytes: &[u8]) -> StdString {
    if bytes.is_ascii() {
        // SAFETY: ASCII is valid UTF-8
        return unsafe { StdString::from_utf8_unchecked(bytes.to_vec()) };
    }
    let non_ascii = bytes.iter().filter(|&&b| b >= 0x80).count();
    let mut out = Vec::with_capacity(bytes.len() + non_ascii);
    for &b in bytes {
        if b < 0x80 {
            out.push(b);
        } else {
            out.extend_from_slice(&[0xc0 | (b >> 6), 0x80 | (b & 0x3f)]);
        }
    }
    // SAFETY: each byte was encoded as a valid UTF-8 sequence
    unsafe { StdString::from_utf8_unchecked(out) }
}

/// Encode UTF-16 code units as UTF-8, or `None` if there is an unpaired surrogate.
fn utf16_to_string(units: &[u16]) -> Option<StdString> {
    // exact UTF-8 length, rejecting unpaired surrogates
    let mut len = 0;
    let mut i = 0;
    while i < units.len() {
        let c = units[i];
        len += match c {
            0..=0x7f => 1,
            0x80..=0x7ff => 2,
            0xd800..=0xdbff => match units.get(i + 1) {
                Some(&c1) if (0xdc00..=0xdfff).contains(&c1) => {
                    i += 1;
                    4
                }
                _ => return None,
            },
            0xdc00..=0xdfff => return None,
            _ => 3,
        };
        i += 1;
    }
    let mut out = Vec::<u8>::with_capacity(len);
    let dst = out.as_mut_ptr();
    let mut pos = 0;
    let mut i = 0;
    // SAFETY: the code units are classified as in the first pass, which
    // counted exactly the bytes written here into the capacity of len
    unsafe {
        while i < units.len() {
            let c = units[i] as u32;
            i += 1;
            if c < 0x80 {
                dst.add(pos).write(c as u8);
                pos += 1;
            } else if c < 0x800 {
                dst.add(pos).write(0xc0 | (c >> 6) as u8);
                dst.add(pos + 1).write(0x80 | (c & 0x3f) as u8);
                pos += 2;
            } else if (0xd800..=0xdbff).contains(&c) {
                // paired, checked by the first pass
                let c = 0x10000 + ((c - 0xd800) << 10) + (units[i] as u32 - 0xdc00);
                i += 1;
                dst.add(pos).write(0xf0 | (c >> 18) as u8);
                dst.add(pos + 1).write(0x80 | ((c >> 12) & 0x3f) as u8);
                dst.add(pos + 2).write(0x80 | ((c >> 6) & 0x3f) as u8);
                dst.add(pos + 3).write(0x80 | (c & 0x3f) as u8);
                pos += 4;
            } else {
                dst.add(pos).write(0xe0 | (c >> 12) as u8);
                dst.add(pos + 1).write(0x80 | ((c >> 6) & 0x3f) as u8);
                dst.add(pos + 2).write(0x80 | (c & 0x3f) as u8);
                pos += 3;
            }
        }
        debug_assert_eq!(pos, len);
        out.set_len(len);
        // SAFETY: UTF-8 encoding of scalar values (surrogates were paired)
        Some(StdString::from_utf8_unchecked(out))
    }
}

/// Rust representation of a JavaScript string.
#[derive(Debug, Clone, PartialEq, Hash)]
#[repr(transparent)]
pub struct String<'js>(pub(crate) Value<'js>);

impl<'js> String<'js> {
    /// Borrow the contents of the string without copying (a rope is flattened
    /// in place). Valid while `self` holds the value since strings are immutable.
    pub(crate) fn data(&self) -> Result<StringData<'_>> {
        let mut len = 0;
        let mut wide = 0;
        let ptr = unsafe {
            qjs::JS_GetStringData(
                self.0.ctx.as_ptr(),
                self.0.as_js_value(),
                &mut len,
                &mut wide,
            )
        };
        if ptr.is_null() {
            // out of memory while flattening a rope
            return Err(Error::Unknown);
        }
        let len = len as usize;
        // SAFETY: the engine returns len code units of the given width, kept
        // alive by the reference held by self
        Ok(unsafe {
            if wide != 0 {
                StringData::Utf16(slice::from_raw_parts(ptr as *const u16, len))
            } else {
                StringData::Latin1(slice::from_raw_parts(ptr as *const u8, len))
            }
        })
    }

    /// Convert the JavaScript string to a Rust string.
    ///
    /// Fails with [`Error::Utf8`] if the string contains an unpaired surrogate.
    pub fn to_string(&self) -> Result<StdString> {
        match self.data()? {
            StringData::Latin1(bytes) => Ok(latin1_to_string(bytes)),
            StringData::Utf16(units) => match utf16_to_string(units) {
                Some(s) => Ok(s),
                None => self.to_string_via_cstring(),
            },
        }
    }

    /// Borrow the string as `&str` when it is ASCII, convert it otherwise.
    ///
    /// Fails with [`Error::Utf8`] if the string contains an unpaired surrogate.
    pub fn to_str(&self) -> Result<Cow<'_, str>> {
        match self.data()? {
            // SAFETY: ASCII is valid UTF-8
            StringData::Latin1(bytes) if bytes.is_ascii() => {
                Ok(Cow::Borrowed(unsafe { str::from_utf8_unchecked(bytes) }))
            }
            StringData::Latin1(bytes) => Ok(Cow::Owned(latin1_to_string(bytes))),
            StringData::Utf16(units) => match utf16_to_string(units) {
                Some(s) => Ok(Cow::Owned(s)),
                None => self.to_string_via_cstring().map(Cow::Owned),
            },
        }
    }

    /// Conversion through the engine's UTF-8 encoder, which keeps unpaired
    /// surrogates as 3 byte sequences: used to report the same [`Error::Utf8`].
    #[cold]
    fn to_string_via_cstring(&self) -> Result<StdString> {
        let mut len = mem::MaybeUninit::uninit();
        let ptr = unsafe {
            qjs::JS_ToCStringLen(self.0.ctx.as_ptr(), len.as_mut_ptr(), self.0.as_js_value())
        };
        if ptr.is_null() {
            // Might not ever happen but I am not 100% sure
            // so just incase check it.
            return Err(Error::Unknown);
        }
        let len = unsafe { len.assume_init() };
        let bytes: &[u8] = unsafe { slice::from_raw_parts(ptr as _, len as _) };
        let result = str::from_utf8(bytes).map(|s| s.into());
        unsafe { qjs::JS_FreeCString(self.0.ctx.as_ptr(), ptr) };
        Ok(result?)
    }

    /// Convert the Javascript string to a Javascript C string.
    pub fn to_cstring(self) -> Result<CString<'js>> {
        CString::from_string(self)
    }

    /// Create a new JavaScript string from an Rust string.
    pub fn from_str(ctx: Ctx<'js>, s: &str) -> Result<Self> {
        let len = s.len();
        let ptr = s.as_ptr();
        Ok(unsafe {
            let js_val = qjs::JS_NewStringLen(ctx.as_ptr(), ptr as _, len as _);
            let js_val = ctx.handle_exception(js_val)?;
            String::from_js_value(ctx, js_val)
        })
    }
}

/// Rust representation of a JavaScript C string.
#[derive(Debug)]
pub struct CString<'js> {
    ptr: NonNull<c_char>,
    len: usize,
    ctx: Ctx<'js>,
}

impl<'js> CString<'js> {
    /// Create a new JavaScript C string from a JavaScript string.
    pub fn from_string(string: String<'js>) -> Result<Self> {
        let mut len = mem::MaybeUninit::uninit();
        // SAFETY: The pointer points to a JSString content which is ref counted
        let ptr = unsafe {
            qjs::JS_ToCStringLen(string.0.ctx.as_ptr(), len.as_mut_ptr(), string.as_raw())
        };
        if ptr.is_null() {
            // Might not ever happen but I am not 100% sure
            // so just incase check it.
            return Err(Error::Unknown);
        }
        let len = unsafe { len.assume_init() };
        Ok(Self {
            ptr: unsafe { NonNull::new_unchecked(ptr as *mut _) },
            len,
            ctx: string.0.ctx.clone(),
        })
    }

    /// Converts a `CString` to a raw pointer.
    pub fn as_ptr(&self) -> *const c_char {
        self.ptr.as_ptr() as *const _
    }

    /// Returns the length of this `CString`, in bytes (not chars or graphemes).
    pub fn len(&self) -> usize {
        self.len
    }

    /// Returns `true` if this `CString` has a length of zero, and `false` otherwise.
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Extracts a string slice containing the entire `CString`.
    pub fn as_str(&self) -> &str {
        // SAFETY: The pointer points to a JSString content which is ref counted
        let bytes = unsafe { slice::from_raw_parts(self.ptr.as_ptr() as *const u8, self.len) };
        // SAFETY: The bytes are garanteed to be valid utf8 by QuickJS
        unsafe { str::from_utf8_unchecked(bytes) }
    }
}

impl<'js> Drop for CString<'js> {
    fn drop(&mut self) {
        unsafe { qjs::JS_FreeCString(self.ctx.as_ptr(), self.ptr.as_ptr()) };
    }
}
impl<'js> AsRef<str> for CString<'js> {
    fn as_ref(&self) -> &str {
        self.as_str()
    }
}
#[cfg(feature = "std")]
impl<'js> AsRef<std::path::Path> for CString<'js> {
    fn as_ref(&self) -> &std::path::Path {
        std::path::Path::new(self.as_str())
    }
}
impl<'js> core::ops::Deref for CString<'js> {
    type Target = str;

    fn deref(&self) -> &Self::Target {
        self.as_str()
    }
}
#[cfg(feature = "std")]
impl<'js> AsRef<std::ffi::OsStr> for CString<'js> {
    fn as_ref(&self) -> &std::ffi::OsStr {
        self.as_str().as_ref()
    }
}
impl<'js> AsRef<[u8]> for CString<'js> {
    fn as_ref(&self) -> &[u8] {
        self.as_str().as_ref()
    }
}
impl<'js> PartialEq for CString<'js> {
    fn eq(&self, other: &Self) -> bool {
        PartialEq::eq(self.as_str(), other.as_str())
    }
}
impl<'js> Eq for CString<'js> {}
impl<'js> core::hash::Hash for CString<'js> {
    fn hash<H: core::hash::Hasher>(&self, state: &mut H) {
        core::hash::Hash::hash(self.as_str(), state)
    }
}

#[cfg(test)]
mod test {
    use crate::{prelude::*, *};
    use alloc::borrow::Cow;
    #[test]
    fn from_javascript() {
        test_with(|ctx| {
            let s: String = ctx.eval(" 'foo bar baz' ").unwrap();
            assert_eq!(s.to_string().unwrap(), "foo bar baz");
        });
    }

    #[test]
    fn to_javascript() {
        test_with(|ctx| {
            let string = String::from_str(ctx.clone(), "foo").unwrap();
            let func: Function = ctx.eval("x =>  x + 'bar'").unwrap();
            let text: StdString = (string,).apply(&func).unwrap();
            assert_eq!(text, "foobar".to_string());
        });
    }

    #[test]
    fn from_javascript_c() {
        test_with(|ctx| {
            let s: CString = ctx.eval(" 'foo bar baz' ").unwrap();
            assert_eq!(s.as_str(), "foo bar baz");
        });
    }

    #[test]
    fn to_javascript_c() {
        test_with(|ctx| {
            let string = String::from_str(ctx.clone(), "foo")
                .unwrap()
                .to_cstring()
                .unwrap();
            let func: Function = ctx.eval("x =>  x + 'bar'").unwrap();
            let text: StdString = (string,).apply(&func).unwrap();
            assert_eq!(text, "foobar".to_string());
        });
    }

    #[test]
    fn rope_string() {
        test_with(|ctx| {
            let val: Value = ctx
                .eval(
                    r#"
                    let s = "";
                    for (let i = 0; i < 10000; i++) s += "Line " + i + "\n";
                    s
                "#,
                )
                .unwrap();

            assert_eq!(val.type_of(), Type::String);
            assert!(val.is_string());
            assert!(val.as_string().is_some());

            let s: StdString = val.as_string().unwrap().to_string().unwrap();
            assert!(s.starts_with("Line 0\n"));
            assert!(s.contains("Line 999\n"));
            assert!(s.len() > 8000);
        });
    }

    /// to_string() must give exactly what the engine's UTF-8 encoder gives
    fn check_same_as_cstring(s: &String) {
        match (s.to_string(), s.to_string_via_cstring()) {
            (Ok(a), Ok(b)) => {
                assert_eq!(a, b);
                assert_eq!(s.to_str().unwrap(), b);
            }
            (Err(Error::Utf8(a)), Err(Error::Utf8(b))) => {
                assert_eq!(a, b);
                assert!(matches!(s.to_str(), Err(Error::Utf8(c)) if c == b));
            }
            (a, b) => panic!("{a:?} != {b:?}"),
        }
    }

    #[test]
    fn to_string_matches_engine() {
        test_with(|ctx| {
            let strings: Vec<String> = ctx
                .eval(
                    r#"
                    const all8 = Array.from({length: 256}, (_, i) => String.fromCharCode(i)).join('');
                    [
                        '', 'a', 'hello world 1234', 'x'.repeat(1024), all8, 'caf\xe9',
                        'h\xe9llo w\xf6rld \u2713\u2713', '\u07ff\u0800\uffff', '\ud83d\ude00 emoji \ud83c\udf89',
                        '\u0100'.repeat(100) + 'a', all8 + '\u20ac' + all8,
                        // ropes, 8 and 16 bit
                        'a'.repeat(1000) + 'b' + 'c'.repeat(1000),
                        'a'.repeat(1000) + '\ud83d\ude00' + 'c'.repeat(1000) + '\xe9',
                        // unpaired surrogates
                        '\ud800', '\udc00', 'ab\ud800', '\ud800x', '\udc00\ud800', 'x\ud83d\ude00\udfff',
                        'a'.repeat(1000) + '\ud800' + 'c'.repeat(1000),
                    ]
                "#,
                )
                .unwrap();
            for s in &strings {
                check_same_as_cstring(s);
            }
            assert!(matches!(strings[0].to_str().unwrap(), Cow::Borrowed("")));
            assert!(matches!(strings[2].to_str().unwrap(), Cow::Borrowed("hello world 1234")));
            assert!(matches!(strings[5].to_str().unwrap(), Cow::Owned(s) if s == "caf\u{e9}"));
            assert_eq!(strings[8].to_string().unwrap(), "\u{1f600} emoji \u{1f389}");
            assert!(matches!(strings[13].to_string(), Err(Error::Utf8(_))));
        });
    }

    #[test]
    fn rope_view_is_stable() {
        test_with(|ctx| {
            let s: String = ctx
                .eval("globalThis.r = 'a'.repeat(1000) + 'b' + 'c'.repeat(1000); r")
                .unwrap();
            assert_eq!(
                unsafe { qjs::JS_VALUE_GET_TAG(s.as_raw()) },
                qjs::JS_TAG_STRING_ROPE
            );
            let view = s.to_str().unwrap();
            assert!(matches!(view, Cow::Borrowed(_)));
            ctx.eval::<(), _>("globalThis.r = undefined; globalThis.r2 = 'x'.repeat(5000);")
                .unwrap();
            ctx.run_gc();
            assert_eq!(view.len(), 2001);
            assert_eq!(&view[999..1002], "abc");
        });
    }

    #[test]
    fn char_from_javascript() {
        test_with(|ctx| {
            assert_eq!(ctx.eval::<char, _>("'\\xe9'").unwrap(), '\u{e9}');
            assert_eq!(ctx.eval::<char, _>("'\\u2713'").unwrap(), '\u{2713}');
            assert_eq!(ctx.eval::<char, _>("'\\ud83d\\ude00'").unwrap(), '\u{1f600}');
            assert!(matches!(ctx.eval::<char, _>("'\\ud83d'"), Err(Error::Utf8(_))));
            assert!(matches!(ctx.eval::<char, _>("'a\\ud83d'"), Err(Error::Utf8(_))));
            assert!(matches!(ctx.eval::<char, _>("'\\u2713a'"), Err(Error::FromJs { .. })));
            assert!(matches!(ctx.eval::<char, _>("'\\xe9a'"), Err(Error::FromJs { .. })));
            assert!(matches!(ctx.eval::<char, _>("''"), Err(Error::FromJs { .. })));
            let c: Value = '\u{1f600}'.into_js(&ctx).unwrap();
            assert_eq!(c.get::<StdString>().unwrap(), "\u{1f600}");
        });
    }
}
