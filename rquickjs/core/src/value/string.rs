use crate::{qjs, Ctx, Error, Result, StdString, Value};
use alloc::{borrow::Cow, vec::Vec};
use core::{
    ffi::c_char,
    mem::{self, MaybeUninit},
    ptr::{self, NonNull},
    slice, str,
};

/// Borrowed contents of a JavaScript string in its internal representation.
pub(crate) enum StringData<'a> {
    /// One Latin-1 (ISO-8859-1) byte per code unit.
    Latin1(&'a [u8]),
    /// UTF-16 code units, possibly with unpaired surrogates.
    Utf16(&'a [u16]),
}

// UTF-8 encoders of the engine's string representations. They count the
// exact output length in a vectorizable pass first. A string of characters
// of one UTF-8 length is encoded with a loop that the compiler vectorizes;
// Latin-1 also in runs of 8 byte words of one length, mixed words and
// mixed UTF-16 strings character by character.

/// Elements per block of the counting passes: the per block counters are
/// `u16` lanes, which vectorize well, and grow by at most 2 per element.
const COUNT_BLOCK: usize = 1 << 14;

/// The high bit of each byte of a word of 8 Latin-1 bytes.
const HIGH_BYTES: u64 = 0x8080_8080_8080_8080;

/// Load 8 bytes as a word (the byte order does not matter to the tests of
/// the high bits).
#[inline(always)]
fn word8(bytes: &[u8], i: usize) -> u64 {
    u64::from_le_bytes(bytes[i..i + 8].try_into().unwrap())
}

/// Number of bytes >= 0x80.
fn count_non_ascii(bytes: &[u8]) -> usize {
    let mut n = 0;
    for block in bytes.chunks(COUNT_BLOCK) {
        let mut count = 0u16;
        for &b in block {
            count += u16::from(b >> 7);
        }
        n += count as usize;
    }
    n
}

/// Copy ASCII bytes to the start of `dst`, returning the number of bytes written.
#[inline(always)]
fn copy_ascii(src: &[u8], dst: &mut [MaybeUninit<u8>]) -> usize {
    let dst = &mut dst[..src.len()];
    // SAFETY: both slices have src.len() elements, MaybeUninit<u8> has the
    // layout of u8, and a shared and a mutable borrow do not overlap
    unsafe { ptr::copy_nonoverlapping(src.as_ptr(), dst.as_mut_ptr().cast::<u8>(), src.len()) };
    src.len()
}

/// Encode Latin-1 bytes which are all >= 0x80 to the start of `dst`,
/// returning the number of bytes written.
fn encode_latin1_high(src: &[u8], dst: &mut [MaybeUninit<u8>]) -> usize {
    let len = src.len() * 2;
    for (d, &b) in dst[..len].chunks_exact_mut(2).zip(src) {
        d[0] = MaybeUninit::new(0xc0 | (b >> 6));
        d[1] = MaybeUninit::new(0x80 | (b & 0x3f));
    }
    len
}

/// Encode Latin-1 bytes one by one at `dst[pos..]`, returning the end position.
#[inline(always)]
fn encode_latin1_bytes(src: &[u8], dst: &mut [MaybeUninit<u8>], mut pos: usize) -> usize {
    for &b in src {
        if b < 0x80 {
            dst[pos] = MaybeUninit::new(b);
            pos += 1;
        } else {
            dst[pos] = MaybeUninit::new(0xc0 | (b >> 6));
            dst[pos + 1] = MaybeUninit::new(0x80 | (b & 0x3f));
            pos += 2;
        }
    }
    pos
}

/// End of the run of words of 8 bytes from `i` whose high bits are `high`.
#[inline(always)]
fn latin1_run_end(bytes: &[u8], mut i: usize, high: u64) -> usize {
    while i + 8 <= bytes.len() && (word8(bytes, i) & HIGH_BYTES) == high {
        i += 8;
    }
    i
}

/// Encode Latin-1 bytes to the start of `dst`, returning the number of bytes
/// written.
fn encode_latin1(src: &[u8], dst: &mut [MaybeUninit<u8>]) -> usize {
    let mut i = 0;
    let mut pos = 0;
    while i + 8 <= src.len() {
        let start = i;
        match word8(src, i) & HIGH_BYTES {
            0 => {
                i = latin1_run_end(src, i + 8, 0);
                pos += copy_ascii(&src[start..i], &mut dst[pos..]);
            }
            HIGH_BYTES => {
                i = latin1_run_end(src, i + 8, HIGH_BYTES);
                pos += encode_latin1_high(&src[start..i], &mut dst[pos..]);
            }
            _ => {
                i += 8;
                pos = encode_latin1_bytes(&src[start..i], dst, pos);
            }
        }
    }
    encode_latin1_bytes(&src[i..], dst, pos)
}

/// Encode Latin-1 bytes as UTF-8.
pub(crate) fn latin1_to_string(bytes: &[u8]) -> StdString {
    if bytes.is_ascii() {
        // SAFETY: ASCII is valid UTF-8
        return unsafe { StdString::from_utf8_unchecked(bytes.to_vec()) };
    }
    let non_ascii = count_non_ascii(bytes);
    let len = bytes.len() + non_ascii;
    let mut out = Vec::<u8>::with_capacity(len);
    let dst = &mut out.spare_capacity_mut()[..len];
    let written = if non_ascii == bytes.len() {
        encode_latin1_high(bytes, dst)
    } else {
        encode_latin1(bytes, dst)
    };
    // the encoders write their output in order from the start of dst
    assert_eq!(written, len);
    // SAFETY: the len bytes are initialized with the UTF-8 encoding of each
    // Latin-1 code point
    unsafe {
        out.set_len(len);
        StdString::from_utf8_unchecked(out)
    }
}

/// Numbers of code units >= U+0080, >= U+0800 and of surrogates.
fn count_utf16(units: &[u16]) -> (usize, usize, usize) {
    let (mut n80, mut n800, mut surrogates) = (0, 0, 0);
    for block in units.chunks(COUNT_BLOCK) {
        let (mut a, mut b, mut c) = (0u16, 0u16, 0u16);
        for &u in block {
            a += u16::from(u >= 0x80);
            b += u16::from(u >= 0x800);
            c += u16::from((u & 0xf800) == 0xd800);
        }
        n80 += a as usize;
        n800 += b as usize;
        surrogates += c as usize;
    }
    (n80, n800, surrogates)
}

/// Encode ASCII code units to the start of `dst`, returning the number of
/// bytes written.
fn narrow_ascii(src: &[u16], dst: &mut [MaybeUninit<u8>]) -> usize {
    for (d, &c) in dst[..src.len()].iter_mut().zip(src) {
        *d = MaybeUninit::new(c as u8);
    }
    src.len()
}

/// Encode code units from U+0080 to U+07FF to the start of `dst`, returning
/// the number of bytes written.
fn encode_two_bytes(src: &[u16], dst: &mut [MaybeUninit<u8>]) -> usize {
    let len = src.len() * 2;
    for (d, &c) in dst[..len].chunks_exact_mut(2).zip(src) {
        d[0] = MaybeUninit::new(0xc0 | (c >> 6) as u8);
        d[1] = MaybeUninit::new(0x80 | (c & 0x3f) as u8);
    }
    len
}

/// Encode code units from U+0800 to U+FFFF which are not surrogates to the
/// start of `dst`, returning the number of bytes written.
fn encode_three_bytes(src: &[u16], dst: &mut [MaybeUninit<u8>]) -> usize {
    let len = src.len() * 3;
    for (d, &c) in dst[..len].chunks_exact_mut(3).zip(src) {
        d[0] = MaybeUninit::new(0xe0 | (c >> 12) as u8);
        d[1] = MaybeUninit::new(0x80 | ((c >> 6) & 0x3f) as u8);
        d[2] = MaybeUninit::new(0x80 | (c & 0x3f) as u8);
    }
    len
}

/// Encode UTF-16 code units one by one to `dst`, returning the number of
/// bytes written, or `None` at an unpaired surrogate.
///
/// # Safety
/// `dst` must be valid for writes of `units.len() * 3` bytes, or of the
/// length computed from [`count_utf16`]: the bytes written for a code unit
/// never exceed what it is counted for (2 for each unit of a surrogate
/// pair), and an unpaired surrogate stops the encoding before any write.
unsafe fn encode_utf16_units(units: &[u16], dst: *mut u8) -> Option<usize> {
    let mut pos = 0;
    let mut i = 0;
    while i < units.len() {
        let c = u32::from(units[i]);
        i += 1;
        // SAFETY: in bounds of dst as required by the caller
        unsafe {
            if c < 0x80 {
                dst.add(pos).write(c as u8);
                pos += 1;
            } else if c < 0x800 {
                dst.add(pos).write(0xc0 | (c >> 6) as u8);
                dst.add(pos + 1).write(0x80 | (c & 0x3f) as u8);
                pos += 2;
            } else if (c & 0xf800) != 0xd800 {
                dst.add(pos).write(0xe0 | (c >> 12) as u8);
                dst.add(pos + 1).write(0x80 | ((c >> 6) & 0x3f) as u8);
                dst.add(pos + 2).write(0x80 | (c & 0x3f) as u8);
                pos += 3;
            } else {
                // a high surrogate followed by a low surrogate
                let c1 = match units.get(i) {
                    Some(&c1) if c < 0xdc00 && (c1 & 0xfc00) == 0xdc00 => u32::from(c1),
                    _ => return None,
                };
                i += 1;
                let c = 0x10000 + ((c - 0xd800) << 10) + (c1 - 0xdc00);
                dst.add(pos).write(0xf0 | (c >> 18) as u8);
                dst.add(pos + 1).write(0x80 | ((c >> 12) & 0x3f) as u8);
                dst.add(pos + 2).write(0x80 | ((c >> 6) & 0x3f) as u8);
                dst.add(pos + 3).write(0x80 | (c & 0x3f) as u8);
                pos += 4;
            }
        }
    }
    Some(pos)
}

/// Strings shorter than this are encoded in one pass into a buffer of the
/// maximum length, without counting their exact length first.
const SHORT_UTF16: usize = 32;

/// Encode UTF-16 code units as UTF-8, or `None` if there is an unpaired surrogate.
pub(crate) fn utf16_to_string(units: &[u16]) -> Option<StdString> {
    let n = units.len();
    if n < SHORT_UTF16 {
        let mut out = Vec::<u8>::with_capacity(n * 3);
        // SAFETY: a code unit takes at most 3 bytes; the written bytes are
        // initialized with the UTF-8 encoding of each code point (surrogates
        // are paired)
        unsafe {
            let len = encode_utf16_units(units, out.as_mut_ptr())?;
            out.set_len(len);
            return Some(StdString::from_utf8_unchecked(out));
        }
    }
    let (n80, n800, surrogates) = count_utf16(units);
    // a surrogate pair takes 4 bytes, the counts give it 3 per code unit
    let len = n + n80 + n800 - surrogates;
    let mut out = Vec::<u8>::with_capacity(len);
    // the whole string at once when all the code units have the same length
    let written = if surrogates == 0 && n80 == 0 {
        narrow_ascii(units, &mut out.spare_capacity_mut()[..len])
    } else if surrogates == 0 && n800 == n {
        encode_three_bytes(units, &mut out.spare_capacity_mut()[..len])
    } else if surrogates == 0 && n80 == n && n800 == 0 {
        encode_two_bytes(units, &mut out.spare_capacity_mut()[..len])
    } else {
        // SAFETY: the capacity is the length counted by count_utf16
        unsafe { encode_utf16_units(units, out.as_mut_ptr())? }
    };
    // the encoders write their output in order from the start of the buffer
    assert_eq!(written, len);
    // SAFETY: the len bytes are initialized with the UTF-8 encoding of each
    // code point (surrogates are paired)
    unsafe {
        out.set_len(len);
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
    /// The source string, converted back to JavaScript without a copy.
    pub(crate) string: String<'js>,
}

impl<'js> CString<'js> {
    /// Create a new JavaScript C string from a JavaScript string.
    ///
    /// Fails with [`Error::Utf8`] if the string contains an unpaired surrogate.
    pub fn from_string(string: String<'js>) -> Result<Self> {
        // Latin-1 always encodes to valid UTF-8, UTF-16 may hold unpaired surrogates
        let wide = matches!(string.data()?, StringData::Utf16(_));
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
        let this = Self {
            ptr: unsafe { NonNull::new_unchecked(ptr as *mut _) },
            len,
            string,
        };
        if wide {
            // the engine keeps unpaired surrogates as invalid 3 byte sequences
            let bytes = unsafe { slice::from_raw_parts(ptr as *const u8, len) };
            str::from_utf8(bytes)?;
        }
        Ok(this)
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
        unsafe { qjs::JS_FreeCString(self.string.0.ctx.as_ptr(), self.ptr.as_ptr()) };
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
                        '\u2713\u2713\u2713\u2713\ud800', '\u2713\u2713\u2713\ud800\u2713\u2713\u2713\u2713',
                        'a'.repeat(1000) + '\ud800' + 'c'.repeat(1000),
                    ]
                "#,
                )
                .unwrap();
            for s in &strings {
                check_same_as_cstring(s);
            }
            assert!(matches!(strings[0].to_str().unwrap(), Cow::Borrowed("")));
            assert!(matches!(
                strings[2].to_str().unwrap(),
                Cow::Borrowed("hello world 1234")
            ));
            assert!(matches!(strings[5].to_str().unwrap(), Cow::Owned(s) if s == "caf\u{e9}"));
            assert_eq!(strings[8].to_string().unwrap(), "\u{1f600} emoji \u{1f389}");
            assert!(matches!(strings[13].to_string(), Err(Error::Utf8(_))));
        });
    }

    #[test]
    fn to_string_matches_engine_mixed() {
        test_with(|ctx| {
            // every length around the word sizes of the encoders, with runs
            // and mixes of characters of each UTF-8 length
            let strings: Vec<String> = ctx
                .eval(
                    r#"
                    const pools = [
                        ['a', 'b'], ['a', '\xe9'], ['\xe9', '\xff', '\x80'], ['a', '\u0100', '\u07ff'],
                        ['\u0800', '\u2713', '\uffff', '\ue000', '\ud7ff'], ['\u0080', '\u07ff'],
                        ['a', '\xe9', '\u2713', '\ud83d\ude00'], ['\ud83d\ude00', '\u2713'],
                        ['a', '\u2713', '\ud800'], ['\u2713', '\udc00'], ['\udbff\udfff', 'a'],
                    ];
                    let seed = 1;
                    const rnd = (n) => {
                        seed = (seed * 1103515245 + 12345) & 0x7fffffff;
                        return seed % n;
                    };
                    const out = [];
                    for (const pool of pools) {
                        for (let len = 0; len < 40; len++) {
                            let s = '';
                            for (let i = 0; i < len; i++) s += pool[rnd(pool.length)];
                            out.push(s);
                            const [first, last] = [pool[0], pool[pool.length - 1]];
                            out.push(first.repeat(len) + last + first.repeat(len));
                            out.push(last.repeat(len) + first + last.repeat(len % 7));
                        }
                    }
                    // more than one block of the counting passes
                    out.push('\xe9'.repeat(40000), '\u2713'.repeat(40000), '\u07ff'.repeat(40000));
                    out.push('a'.repeat(20000) + '\xe9'.repeat(20000) + 'a');
                    out.push('a'.repeat(20000) + '\u2713'.repeat(20000) + '\ud83d\ude00');
                    out
                "#,
                )
                .unwrap();
            for s in &strings {
                check_same_as_cstring(s);
            }
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
    fn cstring_rejects_unpaired_surrogates() {
        test_with(|ctx| {
            let res = ctx.eval::<CString, _>("'ab\\ud800'");
            assert!(matches!(res, Err(Error::Utf8(_))));
            let s: CString = ctx.eval("'h\\xe9llo \\ud83d\\ude00'").unwrap();
            assert_eq!(s.as_str(), "h\u{e9}llo \u{1f600}");
            let func: Function = ctx.eval("x => x === 'h\\xe9llo \\ud83d\\ude00'").unwrap();
            assert!(func.call::<_, bool>((&s,)).unwrap());
            assert!(func.call::<_, bool>((s,)).unwrap());
        });
    }

    #[test]
    fn char_from_javascript() {
        test_with(|ctx| {
            assert_eq!(ctx.eval::<char, _>("'\\xe9'").unwrap(), '\u{e9}');
            assert_eq!(ctx.eval::<char, _>("'\\u2713'").unwrap(), '\u{2713}');
            assert_eq!(
                ctx.eval::<char, _>("'\\ud83d\\ude00'").unwrap(),
                '\u{1f600}'
            );
            assert!(matches!(
                ctx.eval::<char, _>("'\\ud83d'"),
                Err(Error::Utf8(_))
            ));
            assert!(matches!(
                ctx.eval::<char, _>("'a\\ud83d'"),
                Err(Error::Utf8(_))
            ));
            assert!(matches!(
                ctx.eval::<char, _>("'\\u2713a'"),
                Err(Error::FromJs { .. })
            ));
            assert!(matches!(
                ctx.eval::<char, _>("'\\xe9a'"),
                Err(Error::FromJs { .. })
            ));
            assert!(matches!(
                ctx.eval::<char, _>("''"),
                Err(Error::FromJs { .. })
            ));
            let c: Value = '\u{1f600}'.into_js(&ctx).unwrap();
            assert_eq!(c.get::<StdString>().unwrap(), "\u{1f600}");
        });
    }
}
