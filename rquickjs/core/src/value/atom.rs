//!  QuickJS atom functionality.

use crate::{
    qjs,
    value::string::{latin1_to_string, utf16_to_string},
    Ctx, Error, Result, String, Value,
};
use alloc::string::{String as StdString, ToString as _};
use core::{hash::Hash, mem, slice};

mod predefined;
pub use predefined::PredefinedAtom;

/// A QuickJS Atom.
///
/// In QuickJS atoms are similar to interned string but with additional uses.
/// A symbol for instance is just an atom.
///
/// # Representation
///
/// Atoms in QuickJS are handled differently depending on what type of index the represent.
/// When the atom represents a number like index, like `object[1]` the atom is just
/// a normal number.
/// However when the atom represents a string link index like `object["foo"]` or `object.foo`
/// the atom represents a value in a hashmap.
#[derive(Debug)]
pub struct Atom<'js> {
    pub(crate) atom: qjs::JSAtom,
    pub(crate) ctx: Ctx<'js>,
}

impl<'js> PartialEq for Atom<'js> {
    fn eq(&self, other: &Self) -> bool {
        self.atom == other.atom
    }
}
impl<'js> Eq for Atom<'js> {}

impl<'js> Hash for Atom<'js> {
    fn hash<H: core::hash::Hasher>(&self, state: &mut H) {
        state.write_u32(self.atom)
    }
}

impl<'js> Atom<'js> {
    /// Create an atom from a JavaScript value.
    pub fn from_value(ctx: Ctx<'js>, val: &Value<'js>) -> Result<Atom<'js>> {
        let atom = unsafe { qjs::JS_ValueToAtom(ctx.as_ptr(), val.as_js_value()) };
        if atom == qjs::JS_ATOM_NULL {
            // A value can be anything, including an object which might contain a callback so check
            // for panics.
            return Err(ctx.raise_exception());
        }
        Ok(Atom { atom, ctx })
    }

    /// Create an atom from a `u32`
    pub fn from_u32(ctx: Ctx<'js>, val: u32) -> Result<Atom<'js>> {
        let atom = unsafe { qjs::JS_NewAtomUInt32(ctx.as_ptr(), val) };
        if atom == qjs::JS_ATOM_NULL {
            // Should never invoke a callback so no panics
            return Err(Error::Exception);
        }
        Ok(Atom { atom, ctx })
    }

    /// Create an atom from an `i32` via value
    pub fn from_i32(ctx: Ctx<'js>, val: i32) -> Result<Atom<'js>> {
        let atom =
            unsafe { qjs::JS_ValueToAtom(ctx.as_ptr(), qjs::JS_MKVAL(qjs::JS_TAG_INT, val)) };
        if atom == qjs::JS_ATOM_NULL {
            // Should never invoke a callback so no panics
            return Err(Error::Exception);
        }
        Ok(Atom { atom, ctx })
    }

    /// Create an atom from a `bool` via value
    pub fn from_bool(ctx: Ctx<'js>, val: bool) -> Result<Atom<'js>> {
        let val = if val { qjs::JS_TRUE } else { qjs::JS_FALSE };
        let atom = unsafe { qjs::JS_ValueToAtom(ctx.as_ptr(), val) };
        if atom == qjs::JS_ATOM_NULL {
            // Should never invoke a callback so no panics
            return Err(Error::Exception);
        }
        Ok(Atom { atom, ctx })
    }

    /// Create an atom from a `f64` via value
    pub fn from_f64(ctx: Ctx<'js>, val: f64) -> Result<Atom<'js>> {
        let atom = unsafe { qjs::JS_ValueToAtom(ctx.as_ptr(), qjs::JS_NewFloat64(val)) };
        if atom == qjs::JS_ATOM_NULL {
            // Should never invoke a callback so no panics
            return Err(Error::Exception);
        }
        Ok(Atom { atom, ctx })
    }

    /// Create an atom from a Rust string
    pub fn from_str(ctx: Ctx<'js>, name: &str) -> Result<Atom<'js>> {
        unsafe {
            let ptr = name.as_ptr() as *const core::ffi::c_char;
            let atom = qjs::JS_NewAtomLen(ctx.as_ptr(), ptr, name.len() as _);
            if atom == qjs::JS_ATOM_NULL {
                // Should never invoke a callback so no panics
                return Err(Error::Exception);
            }
            Ok(Atom { atom, ctx })
        }
    }

    /// Create an atom from a predefined atom.
    pub fn from_predefined(ctx: Ctx<'js>, predefined: PredefinedAtom) -> Atom<'js> {
        unsafe { Atom::from_atom_val(ctx, predefined as qjs::JSAtom) }
    }

    /// Convert the atom to a Rust string (the description of a symbol).
    ///
    /// Fails with [`Error::Utf8`] if the string contains an unpaired surrogate.
    pub fn to_string(&self) -> Result<StdString> {
        let mut buf = [0; qjs::JS_ATOM_DATA_BUF_SIZE as usize];
        let mut len = 0;
        let mut wide = 0;
        let ptr = unsafe {
            qjs::JS_AtomGetStringData(
                self.ctx.as_ptr(),
                self.atom,
                &mut len,
                &mut wide,
                buf.as_mut_ptr(),
            )
        };
        // SAFETY: `len` code units of the given width, in `buf` or kept alive by the atom
        if wide != 0 {
            let units = unsafe { slice::from_raw_parts(ptr.cast::<u16>(), len as _) };
            match utf16_to_string(units) {
                Some(s) => Ok(s),
                None => self.to_string_via_cstring(),
            }
        } else {
            let bytes = unsafe { slice::from_raw_parts(ptr.cast::<u8>(), len as _) };
            Ok(latin1_to_string(bytes))
        }
    }

    /// Conversion through the engine's UTF-8 encoder, which keeps unpaired surrogates as 3 byte
    /// sequences: used to report the same [`Error::Utf8`] as [`String::to_string`].
    #[cold]
    fn to_string_via_cstring(&self) -> Result<StdString> {
        unsafe {
            let mut len = 0;
            let c_str = qjs::JS_AtomToCStringLen(self.ctx.as_ptr(), &mut len, self.atom);
            if c_str.is_null() {
                return Err(Error::Unknown);
            }
            let bytes = slice::from_raw_parts(c_str as *const u8, len as _);
            let res = core::str::from_utf8(bytes).map(|s| s.to_string());
            qjs::JS_FreeCString(self.ctx.as_ptr(), c_str);
            Ok(res?)
        }
    }

    /// Convert the atom to a JavaScript string.
    pub fn to_js_string(&self) -> Result<String<'js>> {
        unsafe {
            let val = qjs::JS_AtomToString(self.ctx.as_ptr(), self.atom);
            let val = self.ctx.handle_exception(val)?;
            Ok(String::from_js_value(self.ctx.clone(), val))
        }
    }

    /// Convert the atom to a JavaScript value, which will be either a [String] or a [Symbol][crate::Symbol].
    pub fn to_value(&self) -> Result<Value<'js>> {
        unsafe {
            let val = qjs::JS_AtomToValue(self.ctx.as_ptr(), self.atom);
            let val = self.ctx.handle_exception(val)?;
            Ok(Value::from_js_value(self.ctx.clone(), val))
        }
    }

    pub(crate) unsafe fn from_atom_val(ctx: Ctx<'js>, val: qjs::JSAtom) -> Self {
        Atom { atom: val, ctx }
    }

    pub(crate) unsafe fn from_atom_val_dup(ctx: Ctx<'js>, val: qjs::JSAtom) -> Self {
        qjs::JS_DupAtom(ctx.as_ptr(), val);
        Atom { atom: val, ctx }
    }

    /// Take the atom, whose reference the caller then owns.
    pub(crate) fn into_atom_val(self) -> qjs::JSAtom {
        let atom = self.atom;
        unsafe { qjs::JS_FreeContext(self.ctx.as_ptr()) };
        mem::forget(self);
        atom
    }
}

impl<'js> Clone for Atom<'js> {
    fn clone(&self) -> Atom<'js> {
        let atom = unsafe { qjs::JS_DupAtom(self.ctx.as_ptr(), self.atom) };
        Atom {
            atom,
            ctx: self.ctx.clone(),
        }
    }
}

impl<'js> Drop for Atom<'js> {
    fn drop(&mut self) {
        unsafe {
            qjs::JS_FreeAtom(self.ctx.as_ptr(), self.atom);
        }
    }
}

#[cfg(test)]
mod test {
    use crate::*;
    #[test]
    fn to_value() {
        test_with(|ctx| {
            let local_symbol = Symbol::new(ctx.clone()).unwrap();
            for (atom, expected) in [
                (
                    Atom::from_bool(ctx.clone(), false).unwrap(),
                    String::from_str(ctx.clone(), "false").unwrap().into(),
                ),
                (
                    Atom::from_u32(ctx.clone(), 123).unwrap(),
                    String::from_str(ctx.clone(), "123").unwrap().into(),
                ),
                (
                    Atom::from_f64(ctx.clone(), 123.4).unwrap(),
                    String::from_str(ctx.clone(), "123.4").unwrap().into(),
                ),
                (
                    Atom::from_predefined(ctx.clone(), atom::PredefinedAtom::New),
                    String::from_str(ctx.clone(), "new").unwrap().into(),
                ),
                (
                    Atom::from_predefined(ctx.clone(), atom::PredefinedAtom::SymbolToStringTag),
                    ctx.eval("Symbol.toStringTag").unwrap(),
                ),
                (
                    Atom::from_value(ctx.clone(), &local_symbol).unwrap(),
                    local_symbol.into(),
                ),
            ] {
                let to_value = atom.to_value().unwrap();
                assert!(
                    to_value.strict_eq(&expected),
                    "{to_value:?} != {expected:?}",
                );
            }
        });
    }
}
