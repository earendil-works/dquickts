//! Module for types dealing with JS objects.

use crate::{
    convert::FromIteratorJs, qjs, Array, Atom, Ctx, FromAtom, FromJs, IntoAtom, IntoJs, Result,
    Value,
};
use alloc::vec::Vec;
use core::{iter::FusedIterator, marker::PhantomData, mem, mem::MaybeUninit, slice};

mod property;
pub use property::{Accessor, AsProperty, Property, PropertyFlags};

/// Rust representation of a JavaScript object.
#[derive(Debug, PartialEq, Clone, Hash, Eq)]
#[repr(transparent)]
pub struct Object<'js>(pub(crate) Value<'js>);

impl<'js> Object<'js> {
    /// Create a new JavaScript object
    pub fn new(ctx: Ctx<'js>) -> Result<Self> {
        Ok(unsafe {
            let val = qjs::JS_NewObject(ctx.as_ptr());
            let val = ctx.handle_exception(val)?;
            Object::from_js_value(ctx, val)
        })
    }

    /// Create a new JavaScript object with a specified prototype
    ///
    /// This is equivalent to `Object.create(proto)` in JavaScript.
    /// Pass `None` to create an object with a null prototype.
    pub fn new_proto(ctx: Ctx<'js>, proto: Option<&Object<'js>>) -> Result<Self> {
        let proto_val = proto.map(|p| p.0.as_js_value()).unwrap_or(qjs::JS_NULL);
        Ok(unsafe {
            let val = qjs::JS_NewObjectProto(ctx.as_ptr(), proto_val);
            let val = ctx.handle_exception(val)?;
            Object::from_js_value(ctx, val)
        })
    }

    /// Get a new value
    #[inline]
    pub fn get<K: IntoAtom<'js>, V: FromJs<'js>>(&self, k: K) -> Result<V> {
        if let Some(name) = k.as_key_str() {
            // No atom is created for a name which is not an atom yet.
            let val = unsafe {
                qjs::JS_GetPropertyStrLen(
                    self.0.ctx.as_ptr(),
                    self.0.as_js_value(),
                    name.as_ptr().cast(),
                    name.len() as _,
                )
            };
            return unsafe { self.from_get_result(val) };
        }
        let atom = k.into_atom(self.ctx())?;
        self.get_atom(&atom)
    }

    /// Get a value by atom without taking ownership of the atom.
    #[inline]
    pub(crate) fn get_atom<V: FromJs<'js>>(&self, atom: &Atom<'js>) -> Result<V> {
        unsafe {
            let val = qjs::JS_GetProperty(self.0.ctx.as_ptr(), self.0.as_js_value(), atom.atom);
            self.from_get_result(val)
        }
    }

    /// # Safety
    /// `val` must be an owned value returned by a property get on this object's context.
    #[inline]
    unsafe fn from_get_result<V: FromJs<'js>>(&self, val: qjs::JSValue) -> Result<V> {
        let ctx = self.ctx();
        let val = ctx.handle_exception(val)?;
        V::from_js_raw_owned(ctx, val)
    }

    /// check whether the object contains a certain key.
    pub fn contains_key<K>(&self, k: K) -> Result<bool>
    where
        K: IntoAtom<'js>,
    {
        let res = if let Some(name) = k.as_key_str() {
            unsafe {
                qjs::JS_HasPropertyStrLen(
                    self.0.ctx.as_ptr(),
                    self.0.as_js_value(),
                    name.as_ptr().cast(),
                    name.len() as _,
                )
            }
        } else {
            let atom = k.into_atom(self.ctx())?;
            unsafe { qjs::JS_HasProperty(self.0.ctx.as_ptr(), self.0.as_js_value(), atom.atom) }
        };
        if res < 0 {
            return Err(self.0.ctx.raise_exception());
        }
        Ok(res == 1)
    }

    /// Set a member of an object to a certain value
    pub fn set<K: IntoAtom<'js>, V: IntoJs<'js>>(&self, key: K, value: V) -> Result<()> {
        let atom = key.into_atom(self.ctx())?;
        let val = value.into_js(self.ctx())?;
        unsafe {
            if qjs::JS_SetProperty(
                self.0.ctx.as_ptr(),
                self.0.as_js_value(),
                atom.atom,
                val.into_js_value(),
            ) < 0
            {
                return Err(self.0.ctx.raise_exception());
            }
        }
        Ok(())
    }

    /// Remove a member of an object
    pub fn remove<K: IntoAtom<'js>>(&self, key: K) -> Result<()> {
        let atom = key.into_atom(self.ctx())?;
        unsafe {
            if qjs::JS_DeleteProperty(
                self.0.ctx.as_ptr(),
                self.0.as_js_value(),
                atom.atom,
                qjs::JS_PROP_THROW as _,
            ) < 0
            {
                return Err(self.0.ctx.raise_exception());
            }
        }
        Ok(())
    }

    /// Check the object for empty
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Get the number of own string enumerable properties
    ///
    /// Returns 0 if they cannot be enumerated (e.g. a `Proxy` `ownKeys` trap throws); the
    /// exception is left pending in the context, as with [`Object::keys`].
    pub fn len(&self) -> usize {
        let mut len = 0u32;
        unsafe {
            if qjs::JS_GetOwnPropertyCount(
                self.0.ctx.as_ptr(),
                &mut len,
                self.0.as_js_value(),
                Filter::default().flags,
            ) < 0
            {
                // Resume a panic of a Rust callback, the exception stays pending.
                drop(self.0.ctx.raise_exception());
                return 0;
            }
        }
        len as usize
    }

    /// Get own string enumerable property names of an object
    pub fn keys<K: FromAtom<'js>>(&self) -> ObjectKeysIter<'js, K> {
        self.own_keys(Filter::default())
    }

    /// Get own property names of an object
    pub fn own_keys<K: FromAtom<'js>>(&self, filter: Filter) -> ObjectKeysIter<'js, K> {
        ObjectKeysIter {
            state: Some(IterState::new(&self.0, filter.flags)),
            marker: PhantomData,
        }
    }

    /// Get own string enumerable properties of an object
    pub fn props<K: FromAtom<'js>, V: FromJs<'js>>(&self) -> ObjectIter<'js, K, V> {
        self.own_props(Filter::default())
    }

    /// Get own properties of an object
    pub fn own_props<K: FromAtom<'js>, V: FromJs<'js>>(
        &self,
        filter: Filter,
    ) -> ObjectIter<'js, K, V> {
        ObjectIter {
            state: Some(IterState::new(&self.0, filter.flags)),
            object: self.clone(),
            marker: PhantomData,
        }
    }

    /// Get own string enumerable property values of an object
    pub fn values<K: FromAtom<'js>>(&self) -> ObjectValuesIter<'js, K> {
        self.own_values(Filter::default())
    }

    /// Get own property values of an object
    pub fn own_values<K: FromAtom<'js>>(&self, filter: Filter) -> ObjectValuesIter<'js, K> {
        ObjectValuesIter {
            state: Some(IterState::new(&self.0, filter.flags)),
            object: self.clone(),
            marker: PhantomData,
        }
    }

    /// Get an object prototype
    ///
    /// Objects can have no prototype, in this case this function will return null.
    pub fn get_prototype(&self) -> Option<Object<'js>> {
        unsafe {
            let proto = qjs::JS_GetPrototype(self.0.ctx.as_ptr(), self.0.as_js_value());
            if qjs::JS_IsNull(proto) {
                None
            } else {
                Some(Object::from_js_value(self.0.ctx.clone(), proto))
            }
        }
    }

    /// Set an object prototype
    ///
    /// If called with None the function will set the prototype of the object to null.
    ///
    /// This function will error if setting the prototype causes a cycle in the prototype chain.
    pub fn set_prototype(&self, proto: Option<&Object<'js>>) -> Result<()> {
        let proto = proto.map(|x| x.as_js_value()).unwrap_or(qjs::JS_NULL);
        unsafe {
            if 1 != qjs::JS_SetPrototype(self.0.ctx.as_ptr(), self.0.as_js_value(), proto) {
                Err(self.0.ctx.raise_exception())
            } else {
                Ok(())
            }
        }
    }

    /// Check instance of object
    pub fn is_instance_of(&self, class: impl AsRef<Value<'js>>) -> bool {
        let class = class.as_ref();
        0 != unsafe {
            qjs::JS_IsInstanceOf(
                self.0.ctx.as_ptr(),
                self.0.as_js_value(),
                class.as_js_value(),
            )
        }
    }

    /// Convert into an array
    pub fn into_array(self) -> Option<Array<'js>> {
        if self.is_array() {
            Some(Array(self))
        } else {
            None
        }
    }
}

/// The property filter
#[derive(Debug, Clone, Copy)]
#[repr(transparent)]
pub struct Filter {
    flags: qjs::c_int,
}

/// Include only enumerable string properties by default
impl Default for Filter {
    fn default() -> Self {
        Self::new().string().enum_only()
    }
}

impl Filter {
    /// Create filter which includes nothing
    pub fn new() -> Self {
        Self { flags: 0 }
    }

    /// Include string properties
    #[must_use]
    pub fn string(mut self) -> Self {
        self.flags |= qjs::JS_GPN_STRING_MASK as qjs::c_int;
        self
    }

    /// Include symbol properties
    #[must_use]
    pub fn symbol(mut self) -> Self {
        self.flags |= qjs::JS_GPN_SYMBOL_MASK as qjs::c_int;
        self
    }

    /// Include private properties
    #[must_use]
    pub fn private(mut self) -> Self {
        self.flags |= qjs::JS_GPN_PRIVATE_MASK as qjs::c_int;
        self
    }

    /// Include only enumerable properties
    #[must_use]
    pub fn enum_only(mut self) -> Self {
        self.flags |= qjs::JS_GPN_ENUM_ONLY as qjs::c_int;
        self
    }
}

struct IterState<'js> {
    ctx: Ctx<'js>,
    enums: *mut qjs::JSPropertyEnum,
    index: u32,
    count: u32,
}

impl<'js> IterState<'js> {
    fn new(obj: &Value<'js>, flags: qjs::c_int) -> Result<Self> {
        let ctx = obj.ctx();

        let mut enums = mem::MaybeUninit::uninit();
        let mut count = mem::MaybeUninit::uninit();

        let (enums, count) = unsafe {
            if qjs::JS_GetOwnPropertyNames(
                ctx.as_ptr(),
                enums.as_mut_ptr(),
                count.as_mut_ptr(),
                obj.value,
                flags,
            ) < 0
            {
                return Err(ctx.raise_exception());
            }
            let enums = enums.assume_init();
            let count = count.assume_init();
            (enums, count)
        };

        Ok(Self {
            ctx: ctx.clone(),
            enums,
            count,
            index: 0,
        })
    }

    /// Take the next atom, the caller owns its reference.
    fn next_raw(&mut self) -> Option<qjs::JSAtom> {
        if self.index < self.count {
            let elem = unsafe { &*self.enums.offset(self.index as _) };
            self.index += 1;
            Some(elem.atom)
        } else {
            None
        }
    }

    /// Take the last atom, the caller owns its reference.
    fn next_back_raw(&mut self) -> Option<qjs::JSAtom> {
        if self.index < self.count {
            self.count -= 1;
            let elem = unsafe { &*self.enums.offset(self.count as _) };
            Some(elem.atom)
        } else {
            None
        }
    }

    /// Get the property `atom` of `obj` and free the atom taken by `next_raw`/`next_back_raw`.
    fn get_and_free<V: FromJs<'js>>(&self, obj: &Object<'js>, atom: qjs::JSAtom) -> Result<V> {
        unsafe {
            let val = qjs::JS_GetProperty(self.ctx.as_ptr(), obj.0.as_js_value(), atom);
            qjs::JS_FreeAtom(self.ctx.as_ptr(), atom);
            obj.from_get_result(val)
        }
    }
}

impl<'js> Drop for IterState<'js> {
    fn drop(&mut self) {
        // Free atoms which doesn't consumed by the iterator
        for index in self.index..self.count {
            let elem = unsafe { &*self.enums.offset(index as isize) };
            unsafe { qjs::JS_FreeAtom(self.ctx.as_ptr(), elem.atom) };
        }

        // This is safe because iterator cannot outlive ctx
        unsafe { qjs::js_free(self.ctx.as_ptr(), self.enums as _) };
    }
}

impl<'js> Iterator for IterState<'js> {
    type Item = Atom<'js>;

    fn next(&mut self) -> Option<Self::Item> {
        let atom = self.next_raw()?;
        Some(unsafe { Atom::from_atom_val(self.ctx.clone(), atom) })
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        let len = self.len();
        (len, Some(len))
    }
}

impl<'js> DoubleEndedIterator for IterState<'js> {
    fn next_back(&mut self) -> Option<Self::Item> {
        let atom = self.next_back_raw()?;
        Some(unsafe { Atom::from_atom_val(self.ctx.clone(), atom) })
    }
}

impl<'js> ExactSizeIterator for IterState<'js> {
    fn len(&self) -> usize {
        (self.count - self.index) as _
    }
}

impl<'js> FusedIterator for IterState<'js> {}

/// The iterator for an object own keys
pub struct ObjectKeysIter<'js, K> {
    state: Option<Result<IterState<'js>>>,
    marker: PhantomData<K>,
}

impl<'js, K> Iterator for ObjectKeysIter<'js, K>
where
    K: FromAtom<'js>,
{
    type Item = Result<K>;

    fn next(&mut self) -> Option<Self::Item> {
        if let Some(Ok(state)) = &mut self.state {
            match state.next() {
                Some(atom) => Some(K::from_atom(atom)),
                None => {
                    self.state = None;
                    None
                }
            }
        } else if self.state.is_none() {
            None
        } else if let Some(Err(error)) = self.state.take() {
            Some(Err(error))
        } else {
            unreachable!();
        }
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        let len = self.len();
        (len, Some(len))
    }
}

impl<'js, K> DoubleEndedIterator for ObjectKeysIter<'js, K>
where
    K: FromAtom<'js>,
{
    fn next_back(&mut self) -> Option<Self::Item> {
        if let Some(Ok(state)) = &mut self.state {
            match state.next_back() {
                Some(atom) => Some(K::from_atom(atom)),
                None => {
                    self.state = None;
                    None
                }
            }
        } else if self.state.is_none() {
            None
        } else if let Some(Err(error)) = self.state.take() {
            Some(Err(error))
        } else {
            unreachable!();
        }
    }
}

impl<'js, K> ExactSizeIterator for ObjectKeysIter<'js, K>
where
    K: FromAtom<'js>,
{
    fn len(&self) -> usize {
        if let Some(Ok(state)) = &self.state {
            state.len()
        } else {
            0
        }
    }
}

impl<'js, K> FusedIterator for ObjectKeysIter<'js, K> where K: FromAtom<'js> {}

/// The iterator for an object own properties
pub struct ObjectIter<'js, K, V> {
    state: Option<Result<IterState<'js>>>,
    object: Object<'js>,
    marker: PhantomData<(K, V)>,
}

impl<'js, K, V> Iterator for ObjectIter<'js, K, V>
where
    K: FromAtom<'js>,
    V: FromJs<'js>,
{
    type Item = Result<(K, V)>;

    fn next(&mut self) -> Option<Self::Item> {
        if let Some(Ok(state)) = &mut self.state {
            match state.next() {
                // The value is read before the key conversion consumes the atom (no clone).
                Some(atom) => Some(
                    self.object
                        .get_atom(&atom)
                        .and_then(|val| K::from_atom(atom).map(|key| (key, val))),
                ),
                None => {
                    self.state = None;
                    None
                }
            }
        } else if self.state.is_none() {
            None
        } else if let Some(Err(error)) = self.state.take() {
            Some(Err(error))
        } else {
            unreachable!();
        }
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        let len = self.len();
        (len, Some(len))
    }
}

impl<'js, K, V> DoubleEndedIterator for ObjectIter<'js, K, V>
where
    K: FromAtom<'js>,
    V: FromJs<'js>,
{
    fn next_back(&mut self) -> Option<Self::Item> {
        if let Some(Ok(state)) = &mut self.state {
            match state.next_back() {
                // The value is read before the key conversion consumes the atom (no clone).
                Some(atom) => Some(
                    self.object
                        .get_atom(&atom)
                        .and_then(|val| K::from_atom(atom).map(|key| (key, val))),
                ),
                None => {
                    self.state = None;
                    None
                }
            }
        } else if self.state.is_none() {
            None
        } else if let Some(Err(error)) = self.state.take() {
            Some(Err(error))
        } else {
            unreachable!();
        }
    }
}

impl<'js, K, V> ExactSizeIterator for ObjectIter<'js, K, V>
where
    K: FromAtom<'js>,
    V: FromJs<'js>,
{
    fn len(&self) -> usize {
        if let Some(Ok(state)) = &self.state {
            state.len()
        } else {
            0
        }
    }
}

impl<'js, K, V> FusedIterator for ObjectIter<'js, K, V>
where
    K: FromAtom<'js>,
    V: FromJs<'js>,
{
}

/// The iterator for an object own property values
pub struct ObjectValuesIter<'js, V> {
    state: Option<Result<IterState<'js>>>,
    object: Object<'js>,
    marker: PhantomData<V>,
}

impl<'js, V> Iterator for ObjectValuesIter<'js, V>
where
    V: FromJs<'js>,
{
    type Item = Result<V>;

    fn next(&mut self) -> Option<Self::Item> {
        if let Some(Ok(state)) = &mut self.state {
            match state.next_raw() {
                Some(atom) => Some(state.get_and_free(&self.object, atom)),
                None => {
                    self.state = None;
                    None
                }
            }
        } else if self.state.is_none() {
            None
        } else if let Some(Err(error)) = self.state.take() {
            Some(Err(error))
        } else {
            unreachable!();
        }
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        let len = self.len();
        (len, Some(len))
    }
}

impl<'js, V> DoubleEndedIterator for ObjectValuesIter<'js, V>
where
    V: FromJs<'js>,
{
    fn next_back(&mut self) -> Option<Self::Item> {
        if let Some(Ok(state)) = &mut self.state {
            match state.next_back_raw() {
                Some(atom) => Some(state.get_and_free(&self.object, atom)),
                None => {
                    self.state = None;
                    None
                }
            }
        } else if self.state.is_none() {
            None
        } else if let Some(Err(error)) = self.state.take() {
            Some(Err(error))
        } else {
            unreachable!();
        }
    }
}

impl<'js, V> ExactSizeIterator for ObjectValuesIter<'js, V>
where
    V: FromJs<'js>,
{
    fn len(&self) -> usize {
        if let Some(Ok(state)) = &self.state {
            state.len()
        } else {
            0
        }
    }
}

impl<'js, V> FusedIterator for ObjectValuesIter<'js, V> where V: FromJs<'js> {}

impl<'js> IntoIterator for Object<'js> {
    type Item = Result<(Atom<'js>, Value<'js>)>;
    type IntoIter = ObjectIter<'js, Atom<'js>, Value<'js>>;

    fn into_iter(self) -> Self::IntoIter {
        let flags = qjs::JS_GPN_STRING_MASK as _;
        ObjectIter {
            state: Some(IterState::new(&self.0, flags)),
            object: self,
            marker: PhantomData,
        }
    }
}

impl<'js, K, V> FromIteratorJs<'js, (K, V)> for Object<'js>
where
    K: IntoAtom<'js>,
    V: IntoJs<'js>,
{
    type Item = (Atom<'js>, Value<'js>);

    fn from_iter_js<T>(ctx: &Ctx<'js>, iter: T) -> Result<Self>
    where
        T: IntoIterator<Item = (K, V)>,
    {
        let mut builder = ObjectBuilder::new(ctx.clone());
        for (key, value) in iter {
            builder.set(key, value)?;
        }
        builder.build()
    }
}

/// Number of properties of an object being built which are stored without allocation.
const INLINE_PROPS: usize = 16;

/// `JS_NewObjectFrom()` takes at most `i32::MAX` properties.
const MAX_PROPS: usize = i32::MAX as usize;

/// Items of the properties of an object being built, inline up to [`INLINE_PROPS`] items.
struct PropBuf<T: Copy> {
    len: usize,
    inline: [MaybeUninit<T>; INLINE_PROPS],
    /// All the items once there are more than [`INLINE_PROPS`].
    heap: Vec<T>,
}

impl<T: Copy> PropBuf<T> {
    fn new() -> Self {
        PropBuf {
            len: 0,
            inline: [const { MaybeUninit::uninit() }; INLINE_PROPS],
            heap: Vec::new(),
        }
    }

    fn len(&self) -> usize {
        self.len
    }

    fn push(&mut self, item: T) {
        if self.len < INLINE_PROPS {
            self.inline[self.len].write(item);
        } else {
            if self.len == INLINE_PROPS {
                self.heap.reserve(2 * INLINE_PROPS);
                for inline in &self.inline {
                    // SAFETY: the inline items are all initialized
                    self.heap.push(unsafe { inline.assume_init_read() });
                }
            }
            self.heap.push(item);
        }
        self.len += 1;
    }

    fn as_slice(&self) -> &[T] {
        if self.len <= INLINE_PROPS {
            // SAFETY: the first `len` inline items are initialized
            unsafe { slice::from_raw_parts(self.inline.as_ptr().cast::<T>(), self.len) }
        } else {
            &self.heap
        }
    }

    fn clear(&mut self) {
        self.len = 0;
        self.heap.clear();
    }
}

/// Create an object with the properties `atoms[i] = values[i]` defined as by
/// `JS_NewObjectFrom()`, which consumes the values even on failure.
///
/// # Safety
/// The atoms and the owned values must belong to the runtime of `ctx`, at most [`MAX_PROPS`].
unsafe fn new_object_from<'js>(
    ctx: &Ctx<'js>,
    atoms: &[qjs::JSAtom],
    values: &[qjs::JSValue],
) -> Result<Object<'js>> {
    debug_assert!(atoms.len() == values.len() && values.len() <= MAX_PROPS);
    let val = qjs::JS_NewObjectFrom(
        ctx.as_ptr(),
        values.len() as _,
        atoms.as_ptr(),
        values.as_ptr(),
    );
    let val = ctx.handle_exception(val)?;
    Ok(Object::from_js_value(ctx.clone(), val))
}

/// Whether [`Object::set`] of `atom` on a new object of [`Object::new`] without this property
/// only defines it, so that it can be created with the object.
fn set_is_define(ctx: &Ctx<'_>, atom: qjs::JSAtom) -> bool {
    unsafe { qjs::JS_NewObjectSetIsDefine(ctx.as_ptr(), atom) != 0 }
}

/// Set the property `atom` of `object` to the owned `value`.
///
/// # Safety
/// `atom` and `value` must belong to the runtime of `object`.
unsafe fn set_raw(object: &Object<'_>, atom: qjs::JSAtom, value: qjs::JSValue) -> Result<()> {
    let ctx = object.ctx();
    if qjs::JS_SetProperty(ctx.as_ptr(), object.as_js_value(), atom, value) < 0 {
        return Err(ctx.raise_exception());
    }
    Ok(())
}

/// Builder of a new object: the same as [`Object::new`] followed by [`Object::set`] of each
/// property, faster.
///
/// Setting a property of a new object creates it, unless the prototype chain intercepts it with
/// a setter (like `__proto__`, which changes the prototype), a read-only property or an exotic
/// object. While none does, checked for each property when it is set, the properties are kept to
/// create the object with all of them at once and its final shape. From the first intercepted
/// property on, the object is created and the properties are set on it one by one.
///
/// ```
/// # use rquickjs::{Runtime, Context, object::ObjectBuilder};
/// # let rt = Runtime::new().unwrap();
/// # let ctx = Context::full(&rt).unwrap();
/// # ctx.with(|ctx| {
/// let mut builder = ObjectBuilder::new(ctx.clone());
/// builder.set("x", 1)?;
/// builder.set("y", "two")?;
/// let obj = builder.build()?;
/// assert_eq!(obj.get::<_, i32>("x")?, 1);
/// # rquickjs::Result::Ok(())
/// # }).unwrap();
/// ```
pub struct ObjectBuilder<'js> {
    ctx: Ctx<'js>,
    /// The owned atoms of the properties to create with the object.
    atoms: PropBuf<qjs::JSAtom>,
    /// The owned values of the properties to create with the object.
    values: PropBuf<qjs::JSValue>,
    /// The object, once a property must be set on it.
    object: Option<Object<'js>>,
}

impl<'js> ObjectBuilder<'js> {
    /// Start building a new object.
    pub fn new(ctx: Ctx<'js>) -> Self {
        ObjectBuilder {
            ctx,
            atoms: PropBuf::new(),
            values: PropBuf::new(),
            object: None,
        }
    }

    /// Set a property of the object, as [`Object::set`] would.
    pub fn set<K: IntoAtom<'js>, V: IntoJs<'js>>(&mut self, key: K, value: V) -> Result<()> {
        let key = key.into_atom(&self.ctx)?;
        let value = value.into_js(&self.ctx)?;
        if self.object.is_none()
            && self.values.len() < MAX_PROPS
            && set_is_define(&self.ctx, key.atom)
        {
            self.atoms.push(key.into_atom_val());
            self.values.push(value.into_js_value());
            return Ok(());
        }
        self.object()?.set(key, value)
    }

    /// The object, created with the kept properties on first call.
    fn object(&mut self) -> Result<&Object<'js>> {
        if self.object.is_none() {
            let res = unsafe {
                new_object_from(&self.ctx, self.atoms.as_slice(), self.values.as_slice())
            };
            self.values.clear();
            self.free_atoms();
            self.object = Some(res?);
        }
        Ok(self.object.as_ref().unwrap())
    }

    fn free_atoms(&mut self) {
        for &atom in self.atoms.as_slice() {
            unsafe { qjs::JS_FreeAtom(self.ctx.as_ptr(), atom) };
        }
        self.atoms.clear();
    }

    /// Get the object with all its properties.
    pub fn build(mut self) -> Result<Object<'js>> {
        self.object()?;
        Ok(self.object.take().unwrap())
    }
}

impl Drop for ObjectBuilder<'_> {
    fn drop(&mut self) {
        for &value in self.values.as_slice() {
            unsafe { qjs::JS_FreeValue(self.ctx.as_ptr(), value) };
        }
        self.free_atoms();
    }
}

/// Builder of a new object with property names known at compile time, for the derived
/// [`IntoJs`](crate::IntoJs): the same as [`ObjectBuilder`] setting them in order.
///
/// The runtime creates the atoms of the names once and keeps them, while the context keeps the
/// shape of the last objects created with them alive.
#[doc(hidden)]
pub struct StaticObjectBuilder<'js> {
    ctx: Ctx<'js>,
    /// The atoms of the names, kept by the runtime.
    keys: *const [qjs::JSAtom],
    /// The number of properties set.
    count: usize,
    /// The owned values of the first properties, to create with the object.
    values: PropBuf<qjs::JSValue>,
    /// The object, once a property must be set on it.
    object: Option<Object<'js>>,
}

impl<'js> StaticObjectBuilder<'js> {
    /// Start building a new object whose properties will be `names` in order.
    pub fn new(ctx: &Ctx<'js>, names: &'static [&'static str]) -> Result<Self> {
        let keys = unsafe { ctx.get_opaque().static_atoms(ctx, names)? };
        Ok(StaticObjectBuilder {
            ctx: ctx.clone(),
            keys,
            count: 0,
            values: PropBuf::new(),
            object: None,
        })
    }

    fn keys(&self) -> &[qjs::JSAtom] {
        // SAFETY: the runtime keeps the atoms until it is dropped
        unsafe { &*self.keys }
    }

    /// Set the next property, as [`Object::set`] would.
    ///
    /// # Panics
    /// If all the names are already set.
    pub fn set<V: IntoJs<'js>>(&mut self, value: V) -> Result<()> {
        let atom = *self.keys().get(self.count).expect("a value for each name");
        self.count += 1;
        let value = value.into_js(&self.ctx)?;
        if self.object.is_none() && set_is_define(&self.ctx, atom) {
            self.values.push(value.into_js_value());
            return Ok(());
        }
        let object = self.object()?;
        unsafe { set_raw(object, atom, value.into_js_value()) }
    }

    /// The object, created with the kept properties on first call.
    fn object(&mut self) -> Result<&Object<'js>> {
        if self.object.is_none() {
            let values = self.values.as_slice();
            let atoms = &self.keys()[..values.len()];
            let res = unsafe { new_object_from(&self.ctx, atoms, values) };
            self.values.clear();
            self.object = Some(res?);
        }
        Ok(self.object.as_ref().unwrap())
    }

    /// Get the object with all its properties.
    pub fn build(mut self) -> Result<Object<'js>> {
        self.object()?;
        Ok(self.object.take().unwrap())
    }
}

impl Drop for StaticObjectBuilder<'_> {
    fn drop(&mut self) {
        for &value in self.values.as_slice() {
            unsafe { qjs::JS_FreeValue(self.ctx.as_ptr(), value) };
        }
    }
}

#[cfg(test)]
mod test {
    use super::{ObjectBuilder, StaticObjectBuilder};
    use crate::*;

    #[test]
    fn from_javascript() {
        test_with(|ctx| {
            let val: Object = ctx
                .eval(
                    r#"
                let obj = {};
                obj['a'] = 3;
                obj[3] = 'a';
                obj
            "#,
                )
                .unwrap();

            let text: StdString = val.get(3).unwrap();
            assert_eq!(text, "a");
            let int: i32 = val.get("a").unwrap();
            assert_eq!(int, 3);
            let int: StdString = val.get(3).unwrap();
            assert_eq!(int, "a");
            val.set("hallo", "foo").unwrap();
            let text: StdString = val.get("hallo").unwrap();
            assert_eq!(text, "foo".to_string());
            val.remove("hallo").unwrap();
            let text: Option<StdString> = val.get("hallo").unwrap();
            assert_eq!(text, None);
        });
    }

    #[test]
    fn types() {
        test_with(|ctx| {
            let val: Object = ctx
                .eval(
                    r#"
                let array_3 = [];
                array_3[3] = "foo";
                array_3[99] = 4;
                ({
                    array_1: [0,1,2,3,4,5],
                    array_2: [0,"foo",{},undefined,4,5],
                    array_3: array_3,
                    func_1: () => 1,
                    func_2: function(){ return "foo"},
                    obj_1: {
                        a: 1,
                        b: "foo",
                    },
                })
                "#,
                )
                .unwrap();
            assert!(val.get::<_, Object>("array_1").unwrap().is_array());
            assert!(val.get::<_, Object>("array_2").unwrap().is_array());
            assert!(val.get::<_, Object>("array_3").unwrap().is_array());
            assert!(val.get::<_, Object>("func_1").unwrap().is_function());
            assert!(val.get::<_, Object>("func_2").unwrap().is_function());
            assert!(!val.get::<_, Object>("obj_1").unwrap().is_function());
            assert!(!val.get::<_, Object>("obj_1").unwrap().is_array());
        })
    }

    #[test]
    fn own_keys_iter() {
        test_with(|ctx| {
            let val: Object = ctx
                .eval(
                    r#"
                   ({
                     123: 123,
                     str: "abc",
                     arr: [],
                     '': undefined,
                   })
                "#,
                )
                .unwrap();
            let keys = val.keys().collect::<Result<Vec<StdString>>>().unwrap();
            assert_eq!(keys.len(), 4);
            assert_eq!(keys[0], "123");
            assert_eq!(keys[1], "str");
            assert_eq!(keys[2], "arr");
            assert_eq!(keys[3], "");
        })
    }

    #[test]
    fn own_props_iter() {
        test_with(|ctx| {
            let val: Object = ctx
                .eval(
                    r#"
                   ({
                     123: "",
                     str: "abc",
                     '': "def",
                   })
                "#,
                )
                .unwrap();
            let pairs = val
                .props()
                .collect::<Result<Vec<(StdString, StdString)>>>()
                .unwrap();
            assert_eq!(pairs.len(), 3);
            assert_eq!(pairs[0].0, "123");
            assert_eq!(pairs[0].1, "");
            assert_eq!(pairs[1].0, "str");
            assert_eq!(pairs[1].1, "abc");
            assert_eq!(pairs[2].0, "");
            assert_eq!(pairs[2].1, "def");
        })
    }

    #[test]
    fn into_iter() {
        test_with(|ctx| {
            let val: Object = ctx
                .eval(
                    r#"
                   ({
                     123: 123,
                     str: "abc",
                     arr: [],
                     '': undefined,
                   })
                "#,
                )
                .unwrap();
            let pairs = val.into_iter().collect::<Result<Vec<_>>>().unwrap();
            assert_eq!(pairs.len(), 4);
            assert_eq!(pairs[0].0.clone().to_string().unwrap(), "123");
            assert_eq!(i32::from_js(&ctx, pairs[0].1.clone()).unwrap(), 123);
            assert_eq!(pairs[1].0.clone().to_string().unwrap(), "str");
            assert_eq!(StdString::from_js(&ctx, pairs[1].1.clone()).unwrap(), "abc");
            assert_eq!(pairs[2].0.clone().to_string().unwrap(), "arr");
            assert_eq!(Array::from_js(&ctx, pairs[2].1.clone()).unwrap().len(), 0);
            assert_eq!(pairs[3].0.clone().to_string().unwrap(), "");
            assert_eq!(
                Undefined::from_js(&ctx, pairs[3].1.clone()).unwrap(),
                Undefined
            );
        })
    }

    #[test]
    fn iter_take() {
        test_with(|ctx| {
            let val: Object = ctx
                .eval(
                    r#"
                   ({
                     123: 123,
                     str: "abc",
                     arr: [],
                     '': undefined,
                   })
                "#,
                )
                .unwrap();
            let keys = val
                .keys()
                .take(1)
                .collect::<Result<Vec<StdString>>>()
                .unwrap();
            assert_eq!(keys.len(), 1);
            assert_eq!(keys[0], "123");
        })
    }

    #[test]
    fn new_proto_with_empty_prototype() {
        test_with(|ctx| {
            // Object.create({}) equivalent — prototype should be an empty object
            let proto = Object::new(ctx.clone()).unwrap();
            let obj = Object::new_proto(ctx.clone(), Some(&proto)).unwrap();
            obj.set("a", 1).unwrap();

            let got_proto = obj.get_prototype().expect("should have a prototype");
            // The prototype itself should have no own enumerable properties
            assert_eq!(got_proto.keys::<StdString>().count(), 0);
            // But the object's own properties should work
            let v: i32 = obj.get("a").unwrap();
            assert_eq!(v, 1);
        })
    }

    #[test]
    fn new_proto_with_null_prototype() {
        test_with(|ctx| {
            // Object.create(null) equivalent
            let obj = Object::new_proto(ctx.clone(), None).unwrap();
            obj.set("x", 42).unwrap();

            assert!(obj.get_prototype().is_none());
            let v: i32 = obj.get("x").unwrap();
            assert_eq!(v, 42);
        })
    }

    #[test]
    fn new_proto_console_namespace() {
        // Reproduces the exact scenario from issue #572
        test_with(|ctx| {
            let proto = Object::new(ctx.clone()).unwrap();
            let console = Object::new_proto(ctx.clone(), Some(&proto)).unwrap();
            ctx.globals().set("console", console).unwrap();

            let names: Array = ctx
                .eval("Object.getOwnPropertyNames(Object.getPrototypeOf(console))")
                .unwrap();
            assert_eq!(names.len(), 0);
        })
    }

    #[test]
    fn get_by_name() {
        test_with(|ctx| {
            let val: Object = ctx
                .eval(
                    r#"
                    var proto = { get getter() { return this.own * 2 } };
                    var obj = Object.create(proto);
                    obj.own = 21;
                    obj['h\u00e9llo'] = 1;
                    obj['2fa'] = 2;
                    obj['\u{1F600}'] = 3;
                    obj['a\0b'] = 4;
                    obj
                "#,
                )
                .unwrap();
            assert_eq!(val.get::<_, i32>("getter").unwrap(), 42);
            assert_eq!(val.get::<_, i32>("héllo").unwrap(), 1);
            assert_eq!(val.get::<_, i32>(StdString::from("2fa")).unwrap(), 2);
            assert_eq!(val.get::<_, i32>(&StdString::from("😀")).unwrap(), 3);
            assert_eq!(val.get::<_, i32>("a\0b").unwrap(), 4);
            assert_eq!(val.get::<_, Option<i32>>("a").unwrap(), None);
            assert_eq!(val.get::<_, Option<i32>>("hèllo").unwrap(), None);
            assert_eq!(val.get::<_, Option<i32>>("never seen key").unwrap(), None);
            assert_eq!(val.get::<_, Option<i32>>("7 never seen").unwrap(), None);
            assert!(val.contains_key("héllo").unwrap());
            assert!(val.contains_key("getter").unwrap());
            assert!(val.contains_key("toString").unwrap());
            assert!(!val.contains_key("never seen key either").unwrap());
            assert!(!val.contains_key(StdString::from("a")).unwrap());
            let keys = val.keys().collect::<Result<Vec<StdString>>>().unwrap();
            assert_eq!(keys, ["own", "héllo", "2fa", "😀", "a\0b"]);
        })
    }

    #[test]
    fn get_by_name_exotic() {
        test_with(|ctx| {
            let val: Object = ctx
                .eval(
                    r#"
                    var log = [];
                    var px = new Proxy({}, {
                        get(t, k) { log.push(k); return 'p:' + k },
                        has(t, k) { log.push('has:' + k); return k !== 'no' },
                    });
                    px
                "#,
                )
                .unwrap();
            assert_eq!(val.get::<_, StdString>("trap 1 é").unwrap(), "p:trap 1 é");
            assert!(val.contains_key("trap 2").unwrap());
            assert!(!val.contains_key("no").unwrap());
            let child = Object::new_proto(ctx.clone(), Some(&val)).unwrap();
            assert_eq!(child.get::<_, StdString>("trap 3").unwrap(), "p:trap 3");
            assert!(child.contains_key("trap 4").unwrap());
            let log: Vec<StdString> = ctx.eval("log").unwrap();
            assert_eq!(
                log,
                ["trap 1 é", "has:trap 2", "has:no", "trap 3", "has:trap 4"]
            );

            // typed arrays: numeric keys do not reach the prototype
            let ta: Object = ctx
                .eval("Object.prototype['1.5'] = 'proto'; new Uint8Array([5, 6])")
                .unwrap();
            assert_eq!(ta.get::<_, i32>("1").unwrap(), 6);
            assert_eq!(ta.get::<_, Option<i32>>("1.5").unwrap(), None);
            assert_eq!(ta.get::<_, Option<i32>>("-0").unwrap(), None);
            assert!(!ta.contains_key("1.5").unwrap());
            assert!(ta.contains_key("0").unwrap());
            assert_eq!(ta.len(), 2);
            assert!(ctx
                .eval::<bool, _>("delete Object.prototype['1.5']")
                .unwrap());
        })
    }

    #[test]
    fn get_by_name_exotic_class() {
        use crate::class::{
            ClassKind, JsCell, JsClass, PropertyDescriptor, PropertyName, Readable, Trace, Tracer,
        };
        use crate::JsLifetime;

        struct Exotic;

        impl<'js> Trace<'js> for Exotic {
            fn trace<'a>(&self, _tracer: Tracer<'a, 'js>) {}
        }

        unsafe impl<'js> JsLifetime<'js> for Exotic {
            type Changed<'to> = Exotic;
        }

        impl<'js> JsClass<'js> for Exotic {
            const NAME: &'static str = "Exotic";
            const KIND: ClassKind = ClassKind::Exotic;
            type Mutable = Readable;

            fn constructor(_ctx: &Ctx<'js>) -> Result<Option<crate::value::Constructor<'js>>> {
                Ok(None)
            }

            fn exotic_get_property(
                _this: &JsCell<'js, Self>,
                ctx: &Ctx<'js>,
                atom: Atom<'js>,
                _receiver: Value<'js>,
            ) -> Result<Value<'js>> {
                format!("get:{}", atom.to_string()?).into_js(ctx)
            }

            fn exotic_has_property(
                _this: &JsCell<'js, Self>,
                _ctx: &Ctx<'js>,
                atom: Atom<'js>,
            ) -> Result<bool> {
                Ok(atom.to_string()? != "no")
            }

            fn exotic_get_own_property(
                _this: &JsCell<'js, Self>,
                ctx: &Ctx<'js>,
                atom: Atom<'js>,
            ) -> Result<Option<PropertyDescriptor<'js>>> {
                let value = atom.to_string()?.into_js(ctx)?;
                Ok(Some(PropertyDescriptor::new_value(
                    value, true, true, false,
                )))
            }

            fn exotic_get_own_property_names(
                _this: &JsCell<'js, Self>,
                ctx: &Ctx<'js>,
            ) -> Result<Vec<PropertyName<'js>>> {
                ["a", "b", "c"]
                    .into_iter()
                    .map(|name| {
                        Ok(PropertyName {
                            atom: Atom::from_str(ctx.clone(), name)?,
                            is_enumerable: true,
                        })
                    })
                    .collect()
            }
        }

        test_with(|ctx| {
            let obj = Class::instance(ctx.clone(), Exotic).unwrap().into_inner();
            assert_eq!(
                obj.get::<_, StdString>("x never seen é").unwrap(),
                "get:x never seen é"
            );
            assert!(obj.contains_key("y never seen").unwrap());
            assert!(!obj.contains_key("no").unwrap());
            assert_eq!(obj.len(), 3);
            assert!(!obj.is_empty());
            let child = Object::new_proto(ctx.clone(), Some(&obj)).unwrap();
            assert_eq!(
                child.get::<_, StdString>("7 never seen").unwrap(),
                "get:7 never seen"
            );
            assert!(child.contains_key("z never seen").unwrap());
            assert!(child.is_empty());
        })
    }

    #[test]
    fn len_and_is_empty() {
        test_with(|ctx| {
            let len = |src: &str| ctx.eval::<Object, _>(src).unwrap().len();
            assert_eq!(len("({})"), 0);
            assert_eq!(len("({ a: 1, b: 2, 3: 4, [Symbol()]: 5 })"), 3);
            assert_eq!(
                len("Object.defineProperty({ a: 1 }, 'hidden', { value: 2 })"),
                1
            );
            assert_eq!(len("var o = { a: 1, b: 2 }; delete o.a; o"), 1);
            assert_eq!(len("[1, 2, 3]"), 3);
            assert_eq!(len("new String('ab')"), 2);
            assert_eq!(len("new Uint16Array(4)"), 4);
            assert!(ctx.eval::<Object, _>("({})").unwrap().is_empty());
            assert!(!ctx.eval::<Object, _>("[0]").unwrap().is_empty());

            // Proxy traps run as for keys()
            let px: Object = ctx
                .eval(
                    r#"
                    var calls = 0;
                    new Proxy({ a: 1, b: 2 }, {
                        ownKeys(t) { calls++; return Reflect.ownKeys(t) },
                        getOwnPropertyDescriptor(t, k) {
                            calls++;
                            return Reflect.getOwnPropertyDescriptor(t, k)
                        },
                    })
                "#,
                )
                .unwrap();
            assert_eq!(px.len(), 2);
            assert_eq!(ctx.eval::<i32, _>("calls").unwrap(), 3);
            let px: Object = ctx
                .eval("new Proxy({}, { ownKeys() { throw new RangeError('no') } })")
                .unwrap();
            assert_eq!(px.len(), 0);
            assert!(ctx.has_exception());
            drop(ctx.catch());
            assert!(px.is_empty());
            drop(ctx.catch());
        })
    }

    #[test]
    fn values_and_props_iter() {
        test_with(|ctx| {
            let val: Object = ctx
                .eval("({ a: 1, get b() { return 2 }, 'c\\0d': 3 })")
                .unwrap();
            // values::<V>() needs V: FromAtom + FromJs
            let ints = |values: Vec<Value>| -> Vec<Option<i32>> {
                values.iter().map(Value::as_int).collect()
            };
            let values = val.values::<Value>().collect::<Result<Vec<_>>>().unwrap();
            assert_eq!(ints(values), [Some(1), Some(2), Some(3)]);
            let values = val
                .values::<Value>()
                .rev()
                .collect::<Result<Vec<_>>>()
                .unwrap();
            assert_eq!(ints(values), [Some(3), Some(2), Some(1)]);
            let props = val
                .props()
                .rev()
                .collect::<Result<Vec<(StdString, i32)>>>()
                .unwrap();
            let expected: [(StdString, i32); 3] =
                [("c\0d".into(), 3), ("b".into(), 2), ("a".into(), 1)];
            assert_eq!(props, expected);
        })
    }

    /// The own keys, prototype and JSON of `obj` in JavaScript.
    fn describe(ctx: &Ctx<'_>, obj: &Object<'_>) -> StdString {
        ctx.globals().set("described", obj.clone()).unwrap();
        ctx.eval(
            "`${Reflect.ownKeys(described).map(String)} | \
             ${Object.getPrototypeOf(described) === Object.prototype} | \
             ${JSON.stringify(described)}`",
        )
        .unwrap()
    }

    /// `props` set one by one on a new object, and with the builders.
    fn build_all_ways<'js>(
        ctx: &Ctx<'js>,
        props: &[(&str, Value<'js>)],
    ) -> [Result<Object<'js>>; 3] {
        let set = || -> Result<Object<'js>> {
            let obj = Object::new(ctx.clone())?;
            for (key, value) in props {
                obj.set(*key, value.clone())?;
            }
            Ok(obj)
        };
        let built = || -> Result<Object<'js>> {
            let mut builder = ObjectBuilder::new(ctx.clone());
            for (key, value) in props {
                builder.set(*key, value.clone())?;
            }
            builder.build()
        };
        let collected = || props.iter().cloned().collect_js::<Object>(ctx);
        [set(), built(), collected()]
    }

    #[test]
    fn builder_same_as_set() {
        test_with(|ctx| {
            let int = |i: i32| Value::new_int(ctx.clone(), i);
            let proto: Value = ctx.eval("({ inherited: 1 })").unwrap();
            let cases: [&[(&str, Value)]; 6] = [
                &[],
                &[("b", int(0)), ("1", int(1)), ("a", int(2)), ("0", int(3))],
                // a repeated key keeps its position and gets the last value
                &[("a", int(0)), ("b", int(1)), ("a", int(2))],
                // '__proto__' sets the prototype, from the first or a later key
                &[("__proto__", proto.clone()), ("x", int(1))],
                &[("x", int(1)), ("__proto__", proto.clone()), ("y", int(2))],
                // own data properties of Object.prototype are shadowed
                &[("toString", int(1)), ("hasOwnProperty", int(2))],
            ];
            for props in cases {
                let [set, built, collected] = build_all_ways(&ctx, props);
                let expected = describe(&ctx, &set.unwrap());
                assert_eq!(describe(&ctx, &built.unwrap()), expected);
                assert_eq!(describe(&ctx, &collected.unwrap()), expected);
            }
            let [_, built, _] = build_all_ways(&ctx, cases[3]);
            assert_eq!(built.unwrap().get::<_, i32>("inherited").unwrap(), 1);

            // more properties than kept inline
            let keys: Vec<StdString> = (0..40).map(|i| format!("k{i}")).collect();
            let props: Vec<(&str, Value)> = keys.iter().map(|k| (k.as_str(), int(1))).collect();
            let [set, built, collected] = build_all_ways(&ctx, &props);
            let expected = describe(&ctx, &set.unwrap());
            assert_eq!(describe(&ctx, &built.unwrap()), expected);
            assert_eq!(describe(&ctx, &collected.unwrap()), expected);
        })
    }

    #[test]
    fn builder_prototype_setters() {
        test_with(|ctx| {
            let int = |i: i32| Value::new_int(ctx.clone(), i);
            ctx.eval::<(), _>(
                r#"
                var log = [];
                Object.defineProperty(Object.prototype, 'intercepted', {
                    set(v) { log.push(v) }, configurable: true,
                });
                Object.defineProperty(Object.prototype, 'readOnly', { value: 0, configurable: true });
            "#,
            )
            .unwrap();
            let props = [("a", int(1)), ("intercepted", int(2)), ("b", int(3))];
            for obj in build_all_ways(&ctx, &props) {
                assert_eq!(describe(&ctx, &obj.unwrap()), "a,b | true | {\"a\":1,\"b\":3}");
            }
            let log: Vec<i32> = ctx.eval("log").unwrap();
            assert_eq!(log, [2, 2, 2]);

            // a read-only property of the prototype makes the set fail
            let props = [("a", int(1)), ("readOnly", int(2))];
            for obj in build_all_ways(&ctx, &props) {
                assert!(matches!(obj, Err(Error::Exception)));
                let error: StdString = ctx.catch().get::<Object>().unwrap().get("name").unwrap();
                assert_eq!(error, "TypeError");
            }

            // each key is checked when it is set, as Object::set would
            let run = |src: &str| ctx.eval::<(), _>(src).unwrap();
            run("log = []");
            let mut builder = ObjectBuilder::new(ctx.clone());
            builder.set("first", 1).unwrap();
            run(
                r#"
                for (const key of ['first', 'second']) {
                    Object.defineProperty(Object.prototype, key, {
                        set(v) { log.push(key) }, configurable: true,
                    });
                }
            "#,
            );
            builder.set("second", 2).unwrap();
            run("delete Object.prototype.second");
            builder.set("third", 3).unwrap();
            let obj = builder.build().unwrap();
            assert_eq!(
                describe(&ctx, &obj),
                "first,third | true | {\"first\":1,\"third\":3}"
            );
            let log: Vec<StdString> = ctx.eval("log").unwrap();
            assert_eq!(log, ["second"]);
        })
    }

    #[test]
    fn static_builder() {
        static NAMES: [&str; 4] = ["b", "1", "__proto__", "a"];
        fn build(ctx: &Ctx<'_>) -> StdString {
            let mut builder = StaticObjectBuilder::new(ctx, &NAMES).unwrap();
            for i in 0..4 {
                builder.set(i).unwrap();
            }
            let obj = builder.build().unwrap();
            describe(ctx, &obj)
        }
        let rt = Runtime::new().unwrap();
        let ctx1 = Context::full(&rt).unwrap();
        let ctx2 = Context::full(&rt).unwrap();
        for ctx in [&ctx1, &ctx2, &ctx1] {
            ctx.with(|ctx| {
                // '__proto__' sets the prototype to 2: ignored as it isn't an object
                assert_eq!(build(&ctx), "1,b,a | true | {\"1\":1,\"b\":0,\"a\":3}");
            });
        }
        ctx2.with(|ctx| {
            ctx.eval::<(), _>(
                "Object.defineProperty(Object.prototype, 'a', \
                 { set(v) { globalThis.a_set = v }, configurable: true })",
            )
            .unwrap();
            assert_eq!(build(&ctx), "1,b | true | {\"1\":1,\"b\":0}");
            assert_eq!(ctx.globals().get::<_, i32>("a_set").unwrap(), 3);
        });
        // another runtime has its own atoms
        let rt2 = Runtime::new().unwrap();
        Context::full(&rt2).unwrap().with(|ctx| {
            assert_eq!(build(&ctx), "1,b,a | true | {\"1\":1,\"b\":0,\"a\":3}");
        });
        drop((ctx1, ctx2));
        drop(rt);
    }

    #[test]
    fn atom_keys_to_string() {
        test_with(|ctx| {
            let val: Object = ctx
                .eval(
                    r#"({
                        4294967294: 1, 7: 2, 'h\u00e9': 3, '\u{1F600}': 4,
                        [Symbol('desc')]: 5, [Symbol()]: 6, 'a\ud800b': 7, '': 8,
                    })"#,
                )
                .unwrap();
            let keys: Vec<Result<StdString>> =
                val.own_keys(Filter::new().string().symbol()).collect();
            let keys: Vec<_> = keys.into_iter().map(|key| key.ok()).collect();
            let expected = [
                Some("7"),
                Some("4294967294"),
                Some("hé"),
                Some("😀"),
                // an unpaired surrogate isn't UTF-8
                None,
                Some(""),
                Some("desc"),
                Some(""),
            ];
            assert_eq!(keys, expected.map(|key| key.map(StdString::from)));
            let err = val
                .own_keys::<StdString>(Filter::new().string())
                .nth(4)
                .unwrap();
            assert!(matches!(err, Err(Error::Utf8(_))));
        })
    }

    #[test]
    fn collect_js() {
        test_with(|ctx| {
            let object = [("a", "bc"), ("$_", ""), ("", "xyz")]
                .iter()
                .cloned()
                .collect_js::<Object>(&ctx)
                .unwrap();
            assert_eq!(
                StdString::from_js(&ctx, object.get("a").unwrap()).unwrap(),
                "bc"
            );
            assert_eq!(
                StdString::from_js(&ctx, object.get("$_").unwrap()).unwrap(),
                ""
            );
            assert_eq!(
                StdString::from_js(&ctx, object.get("").unwrap()).unwrap(),
                "xyz"
            );
        })
    }
}
