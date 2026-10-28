//! JavaScript array types.

use crate::{atom::PredefinedAtom, qjs, Ctx, FromJs, IntoJs, Object, Result, Value};
use alloc::vec::Vec;
use core::{iter::FusedIterator, marker::PhantomData, mem, ptr};

use super::convert::FromIteratorJs;

/// Rust representation of a JavaScript object optimized as an array.
///
/// JavaScript array's are objects and can be used as such.
/// However arrays in QuickJS are optimized when they do not have any holes.
/// This value represents such an optimized array.
#[derive(Debug, PartialEq, Clone, Eq, Hash)]
#[repr(transparent)]
pub struct Array<'js>(pub(crate) Object<'js>);

impl<'js> Array<'js> {
    /// Create a new JavaScript array.
    pub fn new(ctx: Ctx<'js>) -> Result<Self> {
        Ok(Array(unsafe {
            let val = qjs::JS_NewArray(ctx.as_ptr());
            ctx.handle_exception(val)?;
            Value::from_js_value(ctx, val)
                .into_object()
                .expect("arrays should always be objects")
        }))
    }

    /// Get the length of the JavaScript array.
    pub fn len(&self) -> usize {
        let ctx = self.ctx();
        let value = self.0.as_js_value();
        unsafe {
            let val = qjs::JS_GetProperty(ctx.as_ptr(), value, PredefinedAtom::Length as _);
            assert!(qjs::JS_IsInt(val));
            qjs::JS_VALUE_GET_INT(val) as _
        }
    }

    /// Returns whether a JavaScript array is empty.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Get the value at an index in the JavaScript array.
    pub fn get<V: FromJs<'js>>(&self, idx: usize) -> Result<V> {
        let ctx = self.ctx();
        let obj = self.0.as_js_value();
        let val = unsafe {
            let mut values: *mut qjs::JSValue = ptr::null_mut();
            let mut count: u32 = 0;
            // Elements stored in a fast array are own data properties: reading them
            // directly is what JS_GetPropertyUint32() would do. The pointer is used
            // before any JS code can run.
            if qjs::JS_GetFastArray(ctx.as_ptr(), obj, &mut values, &mut count) != 0
                && idx < count as usize
            {
                Value::from_js_value_const(ctx.clone(), *values.add(idx))
            } else {
                let val = qjs::JS_GetPropertyUint32(ctx.as_ptr(), obj, idx as _);
                let val = ctx.handle_exception(val)?;
                Value::from_js_value(ctx.clone(), val)
            }
        };
        V::from_js(ctx, val)
    }

    /// [`Array::get`] for the list conversions, through `fast_elements()`.
    ///
    /// Measured faster in the conversion loops (`Vec<i32>` of 100: 432 -> 334 ns), slower
    /// as `Array::get` itself (2.9 -> 4.2 ns per element), hence the two copies.
    #[inline]
    pub(crate) fn get_element<V: FromJs<'js>>(&self, idx: usize) -> Result<V> {
        let val = self.get_value(idx)?;
        V::from_js(self.ctx(), val)
    }

    /// Read the element at `idx`, borrowed from the fast array storage when it is there.
    #[inline]
    fn get_value(&self, idx: usize) -> Result<Value<'js>> {
        let ctx = self.ctx();
        let obj = self.0.as_js_value();
        unsafe {
            match fast_elements(ctx, obj) {
                // Elements stored in a fast array are own data properties: reading them
                // directly is what JS_GetPropertyUint32() would do. The pointer is used
                // before any JS code can run.
                Some((values, count)) if idx < count => {
                    Ok(Value::from_js_value_const(ctx.clone(), *values.add(idx)))
                }
                _ => {
                    let val = qjs::JS_GetPropertyUint32(ctx.as_ptr(), obj, idx as _);
                    let val = ctx.handle_exception(val)?;
                    Ok(Value::from_js_value(ctx.clone(), val))
                }
            }
        }
    }

    /// Convert the elements `0..len()` in order, `len()` being read once at the start.
    ///
    /// Like [`Array::iter`], each element is read right before it is converted: a conversion
    /// changing the array is seen by the following elements.
    pub(crate) fn collect_vec<T: FromJs<'js>>(&self) -> Result<Vec<T>> {
        let len = self.len();
        // The stored elements bound the capacity: a long length with holes doesn't inflate it.
        let stored = unsafe { fast_elements(self.ctx(), self.0.as_js_value()) };
        let mut vec = Vec::with_capacity(stored.map_or(0, |(_, count)| count).min(len));
        for idx in 0..len {
            vec.push(self.get_element(idx)?);
        }
        Ok(vec)
    }

    /// Like `Array::from_value`, with a quick accept of fast arrays (which are arrays).
    #[inline]
    pub(crate) fn from_list_value(value: Value<'js>) -> Result<Self> {
        if unsafe { fast_elements(&value.ctx, value.value) }.is_some() {
            Ok(Array(Object(value)))
        } else {
            Self::from_value(value)
        }
    }

    /// Set the value at an index in the JavaScript array.
    pub fn set<V: IntoJs<'js>>(&self, idx: usize, val: V) -> Result<()> {
        let ctx = self.ctx();
        let obj = self.0.as_js_value();
        let val = val.into_js(ctx)?.into_js_value();
        unsafe {
            if 0 > qjs::JS_SetPropertyUint32(ctx.as_ptr(), obj, idx as _, val) {
                return Err(ctx.raise_exception());
            }
        }
        Ok(())
    }

    /// Get an iterator over elements of an array
    pub fn iter<T: FromJs<'js>>(&self) -> ArrayIter<'js, T> {
        let count = self.len() as _;
        ArrayIter {
            array: self.clone(),
            index: 0,
            count,
            marker: PhantomData,
        }
    }

    pub fn into_object(self) -> Object<'js> {
        self.0
    }

    pub fn as_object(&self) -> &Object<'js> {
        &self.0
    }
}

/// The elements and the element count of `obj` when it is a fast array.
///
/// The elements are only valid until JS code runs or the array is changed.
#[inline]
unsafe fn fast_elements(
    ctx: &Ctx<'_>,
    obj: qjs::JSValueConst,
) -> Option<(*const qjs::JSValue, usize)> {
    let mut values: *mut qjs::JSValue = ptr::null_mut();
    let mut count: u32 = 0;
    if qjs::JS_GetFastArray(ctx.as_ptr(), obj, &mut values, &mut count) != 0 {
        Some((values as *const qjs::JSValue, count as usize))
    } else {
        None
    }
}

/// Storage of the converted elements of an [`ArrayBuilder`], owned until the array is made.
pub(crate) trait PendingElements {
    fn as_slice(&self) -> &[qjs::JSValue];
    /// Add a value, there is room for it.
    fn push(&mut self, value: qjs::JSValue);
    /// Forget the values (consumed by the array).
    fn clear(&mut self);
}

impl PendingElements for Vec<qjs::JSValue> {
    fn as_slice(&self) -> &[qjs::JSValue] {
        self
    }

    #[inline]
    fn push(&mut self, value: qjs::JSValue) {
        Vec::push(self, value)
    }

    fn clear(&mut self) {
        Vec::clear(self)
    }
}

/// Room for the `N` elements of a tuple.
pub(crate) struct TupleElements<const N: usize> {
    values: [mem::MaybeUninit<qjs::JSValue>; N],
    len: usize,
}

impl<const N: usize> TupleElements<N> {
    pub(crate) fn new() -> Self {
        Self {
            values: [const { mem::MaybeUninit::uninit() }; N],
            len: 0,
        }
    }
}

impl<const N: usize> PendingElements for TupleElements<N> {
    fn as_slice(&self) -> &[qjs::JSValue] {
        // the first `len` values are initialized
        unsafe { core::slice::from_raw_parts(self.values.as_ptr().cast(), self.len) }
    }

    #[inline]
    fn push(&mut self, value: qjs::JSValue) {
        self.values[self.len].write(value);
        self.len += 1;
    }

    fn clear(&mut self) {
        self.len = 0;
    }
}

/// An array whose elements are set in index order as they are converted, like
/// `Array::new()` followed by `Array::set()` of each element right after its conversion.
///
/// While the prototypes of the arrays can't intercept index sets
/// (`JS_HasStdArrayPrototype()`, checked after each conversion since a conversion can run
/// JS), the sets would only define the elements: the elements are kept and defined at once
/// by `JS_NewArrayFrom()`. From the first element where that doesn't hold on, the array is
/// made from the kept elements and the next ones are set with `JS_SetPropertyUint32()`.
pub(crate) struct ArrayBuilder<'a, 'js, P: PendingElements> {
    ctx: &'a Ctx<'js>,
    pending: P,
    array: Option<Array<'js>>,
    len: usize,
}

impl<'a, 'js, P: PendingElements> ArrayBuilder<'a, 'js, P> {
    /// JS_NewArrayFrom() takes at most i32::MAX values
    const MAX_PENDING: usize = i32::MAX as usize;

    pub(crate) fn new(ctx: &'a Ctx<'js>, pending: P) -> Self {
        Self {
            ctx,
            pending,
            array: None,
            len: 0,
        }
    }

    /// Set the next element.
    #[inline]
    pub(crate) fn push(&mut self, value: Value<'js>) -> Result<()> {
        let value = value.into_js_value();
        let ctx = self.ctx.as_ptr();
        let idx = self.len;
        self.len += 1;
        let array = match &self.array {
            Some(array) => array.0.as_js_value(),
            None => {
                if idx < Self::MAX_PENDING && unsafe { qjs::JS_HasStdArrayPrototype(ctx) } != 0 {
                    self.pending.push(value);
                    return Ok(());
                }
                match self.make_array() {
                    Ok(array) => self.array.insert(array).0.as_js_value(),
                    Err(error) => {
                        unsafe { qjs::JS_FreeValue(ctx, value) };
                        return Err(error);
                    }
                }
            }
        };
        // consumes the value, even on failure
        if unsafe { qjs::JS_SetPropertyUint32(ctx, array, idx as _, value) } < 0 {
            return Err(self.ctx.raise_exception());
        }
        Ok(())
    }

    pub(crate) fn finish(mut self) -> Result<Array<'js>> {
        match self.array.take() {
            Some(array) => Ok(array),
            None => self.make_array(),
        }
    }

    /// Make the array from the pending elements, consumed even on failure.
    fn make_array(&mut self) -> Result<Array<'js>> {
        let ctx = self.ctx;
        let values = self.pending.as_slice();
        unsafe {
            let val = qjs::JS_NewArrayFrom(ctx.as_ptr(), values.len() as _, values.as_ptr());
            self.pending.clear();
            let val = ctx.handle_exception(val)?;
            Ok(Array(Object(Value::from_js_value(ctx.clone(), val))))
        }
    }
}

impl<P: PendingElements> Drop for ArrayBuilder<'_, '_, P> {
    fn drop(&mut self) {
        for &value in self.pending.as_slice() {
            unsafe { qjs::JS_FreeValue(self.ctx.as_ptr(), value) };
        }
    }
}

/// The iterator for an array
pub struct ArrayIter<'js, T> {
    array: Array<'js>,
    index: u32,
    count: u32,
    marker: PhantomData<T>,
}

impl<'js, T> Iterator for ArrayIter<'js, T>
where
    T: FromJs<'js>,
{
    type Item = Result<T>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.index < self.count {
            let res = self.array.get(self.index as _);
            self.index += 1;
            Some(res)
        } else {
            None
        }
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        let len = self.len();
        (len, Some(len))
    }
}

impl<'js, T> DoubleEndedIterator for ArrayIter<'js, T>
where
    T: FromJs<'js>,
{
    fn next_back(&mut self) -> Option<Self::Item> {
        if self.index < self.count {
            self.count -= 1;
            let res = self.array.get(self.count as _);
            Some(res)
        } else {
            None
        }
    }
}

impl<'js, T> ExactSizeIterator for ArrayIter<'js, T>
where
    T: FromJs<'js>,
{
    fn len(&self) -> usize {
        (self.count - self.index) as _
    }
}

impl<'js, T> FusedIterator for ArrayIter<'js, T> where T: FromJs<'js> {}

impl<'js> IntoIterator for Array<'js> {
    type Item = Result<Value<'js>>;
    type IntoIter = ArrayIter<'js, Value<'js>>;

    fn into_iter(self) -> Self::IntoIter {
        let count = self.len() as _;
        ArrayIter {
            array: self,
            index: 0,
            count,
            marker: PhantomData,
        }
    }
}

impl<'js, A> FromIteratorJs<'js, A> for Array<'js>
where
    A: IntoJs<'js>,
{
    type Item = Value<'js>;

    fn from_iter_js<T>(ctx: &Ctx<'js>, iter: T) -> Result<Self>
    where
        T: IntoIterator<Item = A>,
    {
        let iter = iter.into_iter();
        let capacity = iter.size_hint().0.min(i32::MAX as usize);
        let mut array = ArrayBuilder::new(ctx, Vec::with_capacity(capacity));
        for item in iter {
            array.push(item.into_js(ctx)?)?;
        }
        array.finish()
    }
}

#[cfg(test)]
mod test {

    use crate::{convert::List, *};
    #[test]
    fn from_javascript() {
        test_with(|ctx| {
            let val: Array = ctx
                .eval(
                    r#"
                let a = [1,2,3,4,10,"b"]
                a[6] = {}
                a[10] = () => {"hallo"};
                a
                "#,
                )
                .unwrap();
            assert_eq!(val.len(), 11);
            assert_eq!(val.get::<i32>(3).unwrap(), 4);
            assert_eq!(val.get::<i32>(4).unwrap(), 10);
            let _six: Object = val.get(6).unwrap();
        });
    }

    #[test]
    fn into_object() {
        test_with(|ctx| {
            let val: Array = ctx
                .eval(
                    r#"
                let a = [1,2,3];
                a
            "#,
                )
                .unwrap();
            let object = val.into_object();
            assert_eq!(object.get::<_, i32>(0).unwrap(), 1);
        })
    }

    #[test]
    fn into_iter() {
        test_with(|ctx| {
            let val: Array = ctx
                .eval(
                    r#"
                      [1,'abcd',true]
                    "#,
                )
                .unwrap();
            let elems: Vec<_> = val.into_iter().collect::<Result<_>>().unwrap();
            assert_eq!(elems.len(), 3);
            assert_eq!(i8::from_js(&ctx, elems[0].clone()).unwrap(), 1);
            assert_eq!(StdString::from_js(&ctx, elems[1].clone()).unwrap(), "abcd");
            assert!(bool::from_js(&ctx, elems[2].clone()).unwrap());
        })
    }

    #[test]
    fn iter() {
        test_with(|ctx| {
            let val: Array = ctx
                .eval(
                    r#"
                      ["a", 'b', '', "cdef"]
                    "#,
                )
                .unwrap();
            let elems: Vec<StdString> = val.iter().collect::<Result<_>>().unwrap();
            assert_eq!(elems.len(), 4);
            assert_eq!(elems[0], "a");
            assert_eq!(elems[1], "b");
            assert_eq!(elems[2], "");
            assert_eq!(elems[3], "cdef");
        })
    }

    #[test]
    fn collect_js() {
        test_with(|ctx| {
            let array = [1i32, 2, 3]
                .iter()
                .cloned()
                .collect_js::<Array>(&ctx)
                .unwrap();
            assert_eq!(array.len(), 3);
            assert_eq!(i32::from_js(&ctx, array.get(0).unwrap()).unwrap(), 1);
            assert_eq!(i32::from_js(&ctx, array.get(1).unwrap()).unwrap(), 2);
            assert_eq!(i32::from_js(&ctx, array.get(2).unwrap()).unwrap(), 3);
        })
    }

    #[test]
    fn get_holes() {
        test_with(|ctx| {
            let val: Array = ctx
                .eval(
                    r#"
                Array.prototype[3] = 'inherited';
                let a = ['a', 'b'];
                a.length = 5;
                a
                "#,
                )
                .unwrap();
            assert_eq!(val.len(), 5);
            assert_eq!(val.get::<StdString>(1).unwrap(), "b");
            // trailing holes of a fast array go through the prototype chain
            assert_eq!(val.get::<Option<i32>>(2).unwrap(), None);
            assert_eq!(val.get::<StdString>(3).unwrap(), "inherited");
            let elems: Vec<Option<StdString>> = val.iter().collect::<Result<_>>().unwrap();
            assert_eq!(
                elems,
                [Some("a"), Some("b"), None, Some("inherited"), None]
                    .map(|s| s.map(StdString::from))
            );
        })
    }

    #[test]
    fn from_js_sparse() {
        test_with(|ctx| {
            let val: Vec<Option<i32>> = ctx.eval("let s = [1, , 3]; s[6] = 7; s").unwrap();
            assert_eq!(val, [Some(1), None, Some(3), None, None, None, Some(7)]);
            let val: Vec<Option<i32>> = ctx.eval("[]").unwrap();
            assert!(val.is_empty());
        })
    }

    #[test]
    fn from_js_getters() {
        test_with(|ctx| {
            let val: Vec<i32> = ctx
                .eval("Object.defineProperty([1, 2, 3], 1, { get() { return 42 } })")
                .unwrap();
            assert_eq!(val, [1, 42, 3]);
            let src = "Object.defineProperty([1, 2], 0, { get() { throw 'getter' } })";
            let err = ctx.eval::<Vec<i32>, _>(src).unwrap_err();
            assert!(matches!(err, Error::Exception));
            assert_eq!(ctx.catch().get::<StdString>().unwrap(), "getter");
            let val: List<(i32, i32)> = ctx
                .eval("Object.defineProperty([1, 2], 1, { get() { return 5 } })")
                .unwrap();
            assert_eq!(val.0, (1, 5));
        })
    }

    #[test]
    fn from_js_modified_during_conversion() {
        test_with(|ctx| {
            // the conversion of the first element changes and truncates the array:
            // the elements are read when they are converted
            let val: Vec<Option<Coerced<i32>>> = ctx
                .eval(
                    r#"
                let m = [{ valueOf() { m[1] = 5; m.length = 3; return 1 } }, 2, 3, {}];
                m
                "#,
                )
                .unwrap();
            let val: Vec<_> = val.into_iter().map(|v| v.map(|v| v.0)).collect();
            assert_eq!(val, [Some(1), Some(5), Some(3), None]);

            let array: Array = ctx.eval("[1, 2, 3]").unwrap();
            let mut iter = array.iter::<i32>();
            assert_eq!(iter.next().unwrap().unwrap(), 1);
            array.set(1, 20).unwrap();
            assert_eq!(iter.next().unwrap().unwrap(), 20);
            array.set(5, 6).unwrap();
            assert_eq!(iter.next().unwrap().unwrap(), 3);
            assert!(iter.next().is_none());
        })
    }

    #[test]
    fn from_js_collections() {
        use alloc::collections::{BTreeSet, LinkedList, VecDeque};
        use hashbrown::HashSet;
        test_with(|ctx| {
            let fast = "[3, 1, 2, 3]";
            // set past the end and then filled: not a fast array
            let slow = "(() => { const s = [3, 1]; s[3] = 3; s[2] = 2; return s })()";
            for src in [fast, slow] {
                let val: Vec<i32> = ctx.eval(src).unwrap();
                assert_eq!(val, [3, 1, 2, 3]);
                let val: VecDeque<i32> = ctx.eval(src).unwrap();
                assert_eq!(val, [3, 1, 2, 3]);
                let val: LinkedList<i32> = ctx.eval(src).unwrap();
                assert!(val.into_iter().eq([3, 1, 2, 3]));
                let val: BTreeSet<i32> = ctx.eval(src).unwrap();
                assert!(val.into_iter().eq([1, 2, 3]));
                let val: HashSet<i32> = ctx.eval(src).unwrap();
                assert!(val.len() == 3 && (1..=3).all(|i| val.contains(&i)));
                let val: List<(i32, i32, i32, i32)> = ctx.eval(src).unwrap();
                assert_eq!(val.0, (3, 1, 2, 3));
                let val: Vec<Value> = ctx.eval(src).unwrap();
                assert_eq!(val.len(), 4);
                assert_eq!(val[3].as_int(), Some(3));
            }
            assert!(ctx.eval::<BTreeSet<i32>, _>("[1, 'x']").is_err());
            // the capacity comes from the stored elements, not from the length
            let src = "(() => { const h = [1]; h.length = 2 ** 31 - 1; return h })()";
            assert!(ctx.eval::<Vec<i32>, _>(src).is_err());
        })
    }

    #[test]
    fn from_js_not_arrays() {
        test_with(|ctx| {
            assert!(ctx.eval::<Vec<i32>, _>("new Proxy([1, 2], {})").is_err());
            assert!(ctx.eval::<Vec<i32>, _>("new Int32Array([1, 2])").is_err());
            assert!(ctx.eval::<Vec<i32>, _>("({ length: 1, 0: 1 })").is_err());
            assert!(ctx
                .eval::<List<(i32, i32)>, _>("new Proxy([1, 2], {})")
                .is_err());
        })
    }

    #[test]
    fn into_js_lists() {
        test_with(|ctx| {
            let globals = ctx.globals();
            globals.set("v", vec![1, 2, 3]).unwrap();
            globals.set("e", Vec::<i32>::new()).unwrap();
            globals
                .set("r", &vec![StdString::from("a"), StdString::from("b")])
                .unwrap();
            globals.set("s", &[1.5f64, 2.5][..]).unwrap();
            globals.set("t", List((1, "x", true))).unwrap();
            globals.set("t1", List(((),))).unwrap();
            let ok: bool = ctx
                .eval(
                    r#"
                [v, e, r, s, t, t1].every(Array.isArray) &&
                v.join() === '1,2,3' && e.length === 0 && r.join() === 'a,b' &&
                s.join() === '1.5,2.5' && t.join() === '1,x,true' &&
                t1.length === 1 && 0 in t1 &&
                v.push(4) === 4 && v.length === 4 && Object.isExtensible(e)
                "#,
                )
                .unwrap();
            assert!(ok);
        })
    }

    /// Runs a script when converted.
    struct Scripted(i32, &'static str);

    impl<'js> IntoJs<'js> for Scripted {
        fn into_js(self, ctx: &Ctx<'js>) -> Result<Value<'js>> {
            ctx.eval::<(), _>(self.1)?;
            self.0.into_js(ctx)
        }
    }

    #[test]
    fn into_js_inherited_setter() {
        test_with(|ctx| {
            let setter = r#"Object.defineProperty(Array.prototype, "0",
                { set() { throw 123 }, configurable: true })"#;
            ctx.eval::<(), _>(setter).unwrap();
            let err = vec![42].into_js(&ctx).unwrap_err();
            assert!(matches!(err, Error::Exception));
            assert_eq!(ctx.catch().as_int(), Some(123));
            let err = List((42, 1)).into_js(&ctx).unwrap_err();
            assert!(matches!(err, Error::Exception));
            assert_eq!(ctx.catch().as_int(), Some(123));
            let err = [1, 2].into_iter().collect_js::<Array>(&ctx).unwrap_err();
            assert!(matches!(err, Error::Exception));
            assert_eq!(ctx.catch().as_int(), Some(123));

            // without the setter, the elements are set one by one from now on
            ctx.eval::<(), _>("delete Array.prototype[0]").unwrap();
            ctx.globals().set("v", vec![1, 2, 3]).unwrap();
            ctx.globals().set("t", List((4, 5))).unwrap();
            let ok: bool = ctx
                .eval("v.join() === '1,2,3' && t.join() === '4,5' && Object.hasOwn(v, 0)")
                .unwrap();
            assert!(ok);
        })
    }

    #[test]
    fn into_js_inherited_read_only() {
        test_with(|ctx| {
            let read_only = "Object.defineProperty(Object.prototype, '1', { value: 0 })";
            ctx.eval::<(), _>(read_only).unwrap();
            let err = vec![1, 2].into_js(&ctx).unwrap_err();
            assert!(matches!(err, Error::Exception));
            assert!(ctx.catch().is_error());
            assert!(List((1, 2)).into_js(&ctx).is_err());
            ctx.catch();
            // index 0 can still be set
            let val = vec![1].into_js(&ctx).unwrap();
            assert_eq!(val.get::<Vec<i32>>().unwrap(), [1]);
        })
    }

    #[test]
    fn into_js_setter_added_during_conversion() {
        // The element 1 conversion adds setters for the indexes 0 and 2: the element 0 was
        // already set, the element 2 goes through the setter (and isn't defined).
        // The setters log to a string: setting array elements in them would recurse.
        let add = r#"
            globalThis.got = "";
            for (const i of ["0", "2"]) {
                Object.defineProperty(Array.prototype, i,
                    { set(v) { got += i + ":" + v + ";" }, configurable: true });
            }
        "#;
        let check = r#"
            a.length === 2 && Object.hasOwn(a, 0) && a[0] === 0 && a[1] === 1 &&
                !Object.hasOwn(a, 2) && got === "2:2;"
        "#;
        test_with(|ctx| {
            let list = vec![Scripted(0, ""), Scripted(1, add), Scripted(2, "")];
            ctx.globals().set("a", list).unwrap();
            assert!(ctx.eval::<bool, _>(check).unwrap());
        });
        test_with(|ctx| {
            let list = List((Scripted(0, ""), Scripted(1, add), Scripted(2, "")));
            ctx.globals().set("a", list).unwrap();
            assert!(ctx.eval::<bool, _>(check).unwrap());
        });
    }

    #[test]
    fn into_js_conversion_error() {
        test_with(|ctx| {
            let obj = Object::new(ctx.clone()).unwrap();
            let items: Vec<StdResult<Object, Error>> =
                vec![Ok(obj.clone()), Err(Error::Unknown), Ok(obj.clone())];
            assert!(matches!(items.into_js(&ctx), Err(Error::Unknown)));
            let res = (0..3)
                .map(|i| match i {
                    1 => Err(Error::Unknown),
                    _ => Ok(Object::new(ctx.clone()).unwrap()),
                })
                .collect_js::<Array>(&ctx);
            assert!(matches!(res, Err(Error::Unknown)));
            let res = List((obj, StdResult::<i32, Error>::Err(Error::Unknown))).into_js(&ctx);
            assert!(matches!(res, Err(Error::Unknown)));
            // the already converted values are freed (checked when the runtime is freed)
        })
    }
}
