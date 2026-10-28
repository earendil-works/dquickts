//! Heap snapshots: save a whole runtime, including a suspended execution, and rebuild it in
//! another runtime or process.
//!
//! [`Ctx::write_snapshot`] serializes every context, object, closure, pending job and suspended
//! execution (see [`Suspender`](crate::runtime::Suspender)) of a runtime;
//! [`Runtime::read_snapshot`] rebuilds them in an empty runtime of the same build of the
//! program. A [`SnapshotKind::Base`] snapshot (typically taken once the natives are installed
//! and the application script has run) makes the following [`SnapshotKind::Delta`] snapshots
//! small: they only contain what changed since the base.
//!
//! Rust code cannot be serialized, so the Rust functions and classes which a snapshot can
//! reference are registered in a [`SnapshotHost`], identically in the program writing and the
//! program reading the snapshot:
//!
//! - native functions ([`SnapshotHost::function`]), created once per runtime and exposed to
//!   scripts through [`Natives`],
//! - classes ([`SnapshotHost::class`]): their prototype and constructor (with every method,
//!   accessor and static function) are natives too, and their instances are saved with
//!   [`Persist`].
//!
//! Writing a snapshot fails with a `TypeError` if the heap holds another Rust function (e.g. a
//! closure created by a native function) or an instance of an unregistered class. Rust values
//! which hold JavaScript values ([`Persistent`] values, user data...) are not part of the heap:
//! pass the values to keep as snapshot roots.
//!
//! [`SnapshotHost::builtins`] also gives scripts `snapshot()` and `restore()`, which save the
//! execution and jump back to a saved state, like `setjmp()` and `longjmp()`.
//!
//! # Example
//!
//! ```
//! use rquickjs::{
//!     snapshot::{SnapshotHost, SnapshotKind},
//!     Context, Function, Runtime,
//! };
//!
//! let host = SnapshotHost::new().function("add", |ctx| {
//!     Function::new(ctx.clone(), |a: i32, b: i32| a + b)?.with_name("add")
//! });
//!
//! let rt = Runtime::new().unwrap();
//! let ctx = Context::full(&rt).unwrap();
//! let base = ctx.with(|ctx| {
//!     let natives = host.natives(&ctx)?;
//!     ctx.globals().set("add", natives.function("add")?)?;
//!     ctx.eval::<(), _>("var total = 0; function step(n) { total = add(total, n); return total; }")?;
//!     ctx.write_snapshot(&host, SnapshotKind::Base, &[])
//! })
//! .unwrap();
//! let delta = ctx
//!     .with(|ctx| {
//!         ctx.eval::<i32, _>("step(2)")?;
//!         ctx.write_snapshot(&host, SnapshotKind::Delta, &[])
//!     })
//!     .unwrap();
//! assert!(delta.len() < base.len() / 10);
//!
//! // in another runtime (or process)
//! let rt = Runtime::new().unwrap();
//! let restored = rt.read_snapshot(&host, &base, Some(&delta)).unwrap();
//! let total: i32 = restored.contexts[0].with(|ctx| ctx.eval("step(3)")).unwrap();
//! assert_eq!(total, 5);
//! ```

use crate::{
    class::{
        ffi::{class_cell, ClassCell},
        JsCell, JsClass,
    },
    function::{defer_call_job, Opt, RustFunction},
    qjs,
    runtime::opaque::Opaque,
    Array, Class, Constructor, Context, Ctx, Error, Exception, Function, JsLifetime, Object,
    Persistent, Result, Runtime, TypedArray, Value,
};
use alloc::{
    boxed::Box,
    collections::{BTreeMap, BTreeSet},
    format,
    string::String as StdString,
    sync::Arc,
    vec::Vec,
};
use core::{
    any::TypeId,
    ffi::{c_char, c_int, c_void, CStr},
    fmt,
    mem::MaybeUninit,
    panic::AssertUnwindSafe,
    ptr::{self, NonNull},
    slice,
};

#[cfg(test)]
mod test;

/// What [`Ctx::write_snapshot`] saves.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnapshotKind {
    /// The whole runtime.
    Full,
    /// The whole runtime, which also becomes the base of its next [`SnapshotKind::Delta`]
    /// snapshots (replacing the previous base). Until [`Runtime::drop_snapshot_base`], the
    /// runtime keeps every object of the base alive.
    Base,
    /// Only what changed since the base of the runtime: the last [`SnapshotKind::Base`]
    /// snapshot it wrote, or the base snapshot it was restored from. Read it with that base.
    Delta,
}

/// A Rust class whose instances can be saved in snapshots (see [`SnapshotHost::class`]).
pub trait Persist<'js>: JsClass<'js> {
    /// Describes the Rust data of `self` as a JavaScript value. A primitive value keeps the
    /// deltas small: a new object is a change of every delta.
    fn save(&self, ctx: &Ctx<'js>) -> Result<Value<'js>>;

    /// Rebuilds the Rust data from the value returned by [`Persist::save`].
    fn load(ctx: &Ctx<'js>, saved: Value<'js>) -> Result<Self>;
}

type BuildFunction = dyn for<'js> Fn(&Ctx<'js>) -> Result<Function<'js>> + Send + Sync;

/// A registered class, with its lifetime erased.
#[derive(Clone)]
struct ClassEntry {
    name: &'static str,
    id: TypeId,
    prototype: for<'js> fn(&Ctx<'js>) -> Result<Option<Object<'js>>>,
    constructor: for<'js> fn(&Ctx<'js>) -> Result<Option<Constructor<'js>>>,
    save: for<'js> unsafe fn(&Ctx<'js>, NonNull<ClassCell<()>>) -> Result<Value<'js>>,
    load: for<'js> fn(&Ctx<'js>, Value<'js>) -> Result<NonNull<ClassCell<()>>>,
}

#[derive(Clone, Default)]
struct HostInner {
    functions: Vec<(&'static str, Arc<BuildFunction>)>,
    classes: Vec<ClassEntry>,
}

/// The Rust functions and classes that the snapshots of a runtime can reference.
///
/// The program which reads a snapshot must register the same functions and classes, in the
/// same order, as the program which wrote it. The host is cheap to clone.
#[derive(Clone, Default)]
pub struct SnapshotHost(Arc<HostInner>);

impl fmt::Debug for SnapshotHost {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("SnapshotHost")
            .field(
                "functions",
                &self.0.functions.iter().map(|(k, _)| k).collect::<Vec<_>>(),
            )
            .field(
                "classes",
                &self.0.classes.iter().map(|c| c.name).collect::<Vec<_>>(),
            )
            .finish()
    }
}

/// The key of the native function `snapshot()` registered by [`SnapshotHost::builtins`].
pub const SNAPSHOT_BUILTIN: &str = "snapshot";
/// The key of the native function `restore()` registered by [`SnapshotHost::builtins`].
pub const RESTORE_BUILTIN: &str = "restore";

impl SnapshotHost {
    /// Creates a host without natives.
    pub fn new() -> Self {
        Self::default()
    }

    /// Registers the native function `key`, created by `build` once per runtime (see
    /// [`Natives::function`]). `build` runs again, in the same context, when a snapshot is
    /// read: the Rust code of the functions it creates (and of the functions reachable through
    /// their properties) replaces the code of the saved ones, so it must create them
    /// identically and have no other effect. Their state must live in the heap (use the
    /// [`Ctx`] and the arguments given to each call): JavaScript values captured by a Rust
    /// closure are not saved, a restored function would capture the values created by the
    /// rebuild.
    ///
    /// # Panics
    /// If `key` is already registered.
    pub fn function<F>(mut self, key: &'static str, build: F) -> Self
    where
        F: for<'js> Fn(&Ctx<'js>) -> Result<Function<'js>> + Send + Sync + 'static,
    {
        let inner = Arc::make_mut(&mut self.0);
        assert!(
            inner.functions.iter().all(|(k, _)| *k != key),
            "the native function `{key}` is already registered"
        );
        inner.functions.push((key, Arc::new(build)));
        self
    }

    /// Registers the Rust class `C`: its prototype and constructor become natives (see
    /// [`Natives::constructor`]) and its instances are saved with [`Persist`].
    ///
    /// # Panics
    /// If a class with the same name is already registered.
    pub fn class<C>(mut self) -> Self
    where
        C: JsLifetime<'static> + 'static,
        for<'js> C::Changed<'js>: Persist<'js>,
    {
        let name = <C::Changed<'static> as JsClass<'static>>::NAME;
        let inner = Arc::make_mut(&mut self.0);
        assert!(
            inner.classes.iter().all(|c| c.name != name),
            "the class `{name}` is already registered"
        );
        inner.classes.push(ClassEntry {
            name,
            id: TypeId::of::<C::Changed<'static>>(),
            prototype: class_prototype::<C>,
            constructor: class_constructor::<C>,
            save: class_save::<C>,
            load: class_load::<C>,
        });
        self
    }

    /// Registers the natives `snapshot` and `restore` (see [`SNAPSHOT_BUILTIN`] and
    /// [`RESTORE_BUILTIN`]), to expose to scripts like other natives:
    ///
    /// - `snapshot()` saves the runtime as if the call were still running and returns the
    ///   state, a `Uint8Array`. `snapshot(path)` writes the state to the file `path` and returns
    ///   `undefined`.
    /// - `restore(state, value = true)` abandons the running execution (its `catch` and
    ///   `finally` clauses do not run) and continues from a state saved by `snapshot()`, given
    ///   as a `Uint8Array` or as the path of a file: that `snapshot()` call returns `value` (a
    ///   value supported by the bytecode serializer: no function).
    ///
    /// The host handles a `restore()` call: the call which started the execution
    /// ([`Ctx::eval`], [`Function::call`], [`Ctx::resume`]...) returns [`Error::Restore`],
    /// whose [`Restore::apply`] rebuilds the state in a new runtime, where [`Ctx::resume`]
    /// continues it.
    ///
    /// Both functions can only be called where the execution can be suspended (see
    /// [`Suspender`](crate::runtime::Suspender)): from plain JavaScript functions run by the
    /// host, not from callbacks, getters, async functions, generators or promise jobs.
    /// Elsewhere they throw a `TypeError`. The states are self-contained (full) snapshots.
    pub fn builtins(self) -> Self {
        self.function(SNAPSHOT_BUILTIN, |ctx| {
            Function::new(ctx.clone(), snapshot_builtin)?.with_name(SNAPSHOT_BUILTIN)
        })
        .function(RESTORE_BUILTIN, |ctx| {
            Function::new(ctx.clone(), restore_builtin)?.with_name(RESTORE_BUILTIN)
        })
    }

    /// Returns the natives of the runtime of `ctx`, created on first use.
    ///
    /// # Errors
    /// [`Error::Snapshot`] if the runtime uses the natives of another host.
    pub fn natives<'js>(&self, ctx: &Ctx<'js>) -> Result<Natives<'js>> {
        let opaque = unsafe { ctx.get_opaque() };
        let tree = match opaque.snapshot_natives() {
            Some(tree) => {
                if tree.get::<_, StdString>("signature")? != self.signature() {
                    return Err(Error::Snapshot(
                        "the runtime uses the natives of another SnapshotHost".into(),
                    ));
                }
                tree
            }
            None => {
                let tree = build_natives(self, ctx)?;
                opaque.set_snapshot_natives(Some((tree.clone(), self.clone())));
                tree
            }
        };
        Ok(Natives {
            host: self.clone(),
            tree,
        })
    }

    /// Identifies the registered natives: snapshots are only read with the same natives.
    fn signature(&self) -> StdString {
        let mut signature = StdString::new();
        for (key, _) in &self.0.functions {
            signature.push_str("f:");
            signature.push_str(key);
            signature.push('\n');
        }
        for class in &self.0.classes {
            signature.push_str("c:");
            signature.push_str(class.name);
            signature.push('\n');
        }
        signature
    }

    fn class_index(&self, id: TypeId) -> Option<usize> {
        self.0.classes.iter().position(|c| c.id == id)
    }
}

/// The natives of a runtime (see [`SnapshotHost::natives`]): the objects to expose to scripts.
#[derive(Clone, Debug)]
pub struct Natives<'js> {
    host: SnapshotHost,
    /// `{ functions, classes: [{ prototype, constructor }], table, signature }`, a snapshot
    /// root. `table` lists the Rust functions in a deterministic order: a saved Rust function
    /// is its index in it.
    tree: Object<'js>,
}

impl<'js> Natives<'js> {
    /// Returns the native function `key`.
    ///
    /// # Errors
    /// [`Error::Snapshot`] if `key` is not registered.
    pub fn function(&self, key: &str) -> Result<Function<'js>> {
        let index = self
            .host
            .0
            .functions
            .iter()
            .position(|(k, _)| *k == key)
            .ok_or_else(|| Error::Snapshot(format!("no native function `{key}`")))?;
        self.tree.get::<_, Array>("functions")?.get(index)
    }

    /// Returns the constructor of the registered class `C`.
    ///
    /// # Errors
    /// [`Error::Snapshot`] if `C` is not registered or has no constructor.
    pub fn constructor<C: JsClass<'js>>(&self) -> Result<Constructor<'js>> {
        let index = self
            .host
            .class_index(TypeId::of::<C::Changed<'static>>())
            .ok_or_else(|| Error::Snapshot(format!("the class `{}` is not registered", C::NAME)))?;
        let node: Object = self.tree.get::<_, Array>("classes")?.get(index)?;
        node.get::<_, Option<Constructor>>("constructor")?
            .ok_or_else(|| Error::Snapshot(format!("the class `{}` has no constructor", C::NAME)))
    }
}

/// A runtime rebuilt by [`Runtime::read_snapshot`].
pub struct Restored {
    /// The contexts of the runtime, in creation order.
    pub contexts: Vec<Context>,
    /// The `roots` given to [`Ctx::write_snapshot`].
    pub roots: Vec<Persistent<Value<'static>>>,
}

/// A `restore()` call of a script (see [`SnapshotHost::builtins`]), returned as
/// [`Error::Restore`]: the execution must continue from a saved state.
pub struct Restore {
    state: Vec<u8>,
    /// The value given to `restore()`, serialized with `JS_WriteObject()`.
    value: Vec<u8>,
}

impl fmt::Debug for Restored {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Restored")
            .field("contexts", &self.contexts.len())
            .field("roots", &self.roots.len())
            .finish()
    }
}

impl fmt::Debug for Restore {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Restore")
            .field("state", &self.state.len())
            .field("value", &self.value.len())
            .finish()
    }
}

impl Restore {
    /// The saved state to continue from.
    pub fn state(&self) -> &[u8] {
        &self.state
    }

    /// Rebuilds the state in `runtime`, an empty runtime (see [`Runtime::read_snapshot`]). The
    /// next [`Ctx::resume`] continues the execution which saved it: its `snapshot()` call
    /// returns the value given to `restore()`.
    pub fn apply(self, host: &SnapshotHost, runtime: &Runtime) -> Result<Restored> {
        let restored = runtime.read_snapshot(host, &self.state, None)?;
        restored.contexts[0].with(|ctx| -> Result<()> {
            let value = unsafe {
                let value = qjs::JS_ReadObject(
                    ctx.as_ptr(),
                    self.value.as_ptr(),
                    self.value.len() as _,
                    qjs::JS_READ_OBJ_REFERENCE as c_int,
                );
                Value::from_js_value(ctx.clone(), ctx.handle_exception(value)?)
            };
            unsafe { ctx.get_opaque().set_resume_value(Some(value)) };
            Ok(())
        })?;
        Ok(restored)
    }
}

/// A request of a builtin, carried out once the execution is suspended.
pub(crate) enum BuiltinRequest {
    /// `snapshot(path)`: save the runtime, then resume with the state.
    Snapshot(Option<StdString>),
    /// `restore(state, value)`: return [`Error::Restore`].
    Restore(Restore),
}

/// The C function pointers that rquickjs stores in the heap.
fn native_pointers() -> [*const c_void; 1] {
    [defer_call_job as *const c_void]
}

/// The engine view of the natives, without opaque data hooks.
fn c_host(natives: &[*const c_void], opaque: *mut c_void) -> qjs::JSSnapshotHost {
    qjs::JSSnapshotHost {
        natives: natives.as_ptr(),
        native_count: natives.len() as c_int,
        func_lists: ptr::null(),
        func_list_lens: ptr::null(),
        func_list_count: 0,
        save_opaque: None,
        load_opaque: None,
        opaque,
    }
}

fn object_ptr(value: &Value) -> usize {
    unsafe { qjs::JS_VALUE_GET_PTR(value.as_js_value()) as usize }
}

fn class_prototype<'js, C>(ctx: &Ctx<'js>) -> Result<Option<Object<'js>>>
where
    C: JsLifetime<'static>,
    for<'a> C::Changed<'a>: Persist<'a>,
{
    Class::<C::Changed<'js>>::prototype(ctx)
}

fn class_constructor<'js, C>(ctx: &Ctx<'js>) -> Result<Option<Constructor<'js>>>
where
    C: JsLifetime<'static>,
    for<'a> C::Changed<'a>: Persist<'a>,
{
    Class::<C::Changed<'js>>::create_constructor(ctx)
}

unsafe fn class_save<'js, C>(ctx: &Ctx<'js>, cell: NonNull<ClassCell<()>>) -> Result<Value<'js>>
where
    C: JsLifetime<'static>,
    for<'a> C::Changed<'a>: Persist<'a>,
{
    let cell = cell.cast::<ClassCell<JsCell<'js, C::Changed<'js>>>>();
    let data = cell
        .as_ref()
        .data
        .try_borrow()
        .map_err(Error::ClassBorrow)?;
    data.save(ctx)
}

fn class_load<'js, C>(ctx: &Ctx<'js>, saved: Value<'js>) -> Result<NonNull<ClassCell<()>>>
where
    C: JsLifetime<'static>,
    for<'a> C::Changed<'a>: Persist<'a>,
{
    let data = <C::Changed<'js> as Persist<'js>>::load(ctx, saved)?;
    // owned by the object it is attached to: freed by the class finalizer (like
    // `Class::instance`)
    let cell = Box::into_raw(Box::new(ClassCell::new(data)));
    // SAFETY: `Box::into_raw` never returns null
    Ok(unsafe { NonNull::new_unchecked(cell) }.cast())
}

fn is_rust_function(value: &Value) -> bool {
    unsafe {
        let id = Opaque::callable_id();
        qjs::JS_GetClassID(value.as_js_value()) == id
            && class_cell(value.as_js_value(), id)
                .is_some_and(|cell| cell.as_ref().v_table.is_of_class::<RustFunction>())
    }
}

/// The values of the own properties (getters and setters included) of `obj`.
fn own_property_values<'js>(ctx: &Ctx<'js>, obj: &Object<'js>) -> Result<Vec<Value<'js>>> {
    let mut values = Vec::new();
    unsafe {
        let mut tab = ptr::null_mut();
        let mut len = 0;
        if qjs::JS_GetOwnPropertyNames(
            ctx.as_ptr(),
            &mut tab,
            &mut len,
            obj.as_js_value(),
            (qjs::JS_GPN_STRING_MASK | qjs::JS_GPN_SYMBOL_MASK) as c_int,
        ) < 0
        {
            return Err(ctx.raise_exception());
        }
        let mut result = Ok(());
        for i in 0..len as usize {
            let mut desc = MaybeUninit::<qjs::JSPropertyDescriptor>::uninit();
            let found = qjs::JS_GetOwnProperty(
                ctx.as_ptr(),
                desc.as_mut_ptr(),
                obj.as_js_value(),
                (*tab.add(i)).atom,
            );
            if found < 0 {
                result = Err(ctx.raise_exception());
                break;
            }
            if found > 0 {
                let desc = desc.assume_init();
                for v in [desc.value, desc.getter, desc.setter] {
                    values.push(Value::from_js_value(ctx.clone(), v));
                }
            }
        }
        qjs::JS_FreePropertyEnum(ctx.as_ptr(), tab, len);
        result?;
    }
    Ok(values)
}

/// Appends to `table` the Rust functions reachable from `value` through the properties of
/// functions (and of `value` itself if `expand`), in a deterministic order.
fn collect_rust_functions<'js>(
    ctx: &Ctx<'js>,
    value: Value<'js>,
    expand: bool,
    seen: &mut BTreeSet<usize>,
    table: &mut Vec<Value<'js>>,
) -> Result<()> {
    let Some(obj) = value.as_object() else {
        return Ok(());
    };
    if !seen.insert(object_ptr(&value)) {
        return Ok(());
    }
    if is_rust_function(&value) {
        table.push(value.clone());
    }
    if expand || value.is_function() {
        for v in own_property_values(ctx, obj)? {
            collect_rust_functions(ctx, v, false, seen, table)?;
        }
    }
    Ok(())
}

/// Creates the natives of `host` in the context `ctx`.
fn build_natives<'js>(host: &SnapshotHost, ctx: &Ctx<'js>) -> Result<Object<'js>> {
    let functions = Array::new(ctx.clone())?;
    for (i, (_, build)) in host.0.functions.iter().enumerate() {
        functions.set(i, build(ctx)?)?;
    }
    let classes = Array::new(ctx.clone())?;
    for (i, class) in host.0.classes.iter().enumerate() {
        let node = Object::new(ctx.clone())?;
        node.set("prototype", (class.prototype)(ctx)?)?;
        node.set("constructor", (class.constructor)(ctx)?)?;
        classes.set(i, node)?;
    }

    let mut seen = BTreeSet::new();
    let mut table = Vec::new();
    for f in functions.iter::<Value>() {
        collect_rust_functions(ctx, f?, false, &mut seen, &mut table)?;
    }
    for node in classes.iter::<Object>() {
        let node = node?;
        collect_rust_functions(ctx, node.get("prototype")?, true, &mut seen, &mut table)?;
        collect_rust_functions(ctx, node.get("constructor")?, false, &mut seen, &mut table)?;
    }

    let tree = Object::new(ctx.clone())?;
    tree.set("functions", functions)?;
    tree.set("classes", classes)?;
    let table_array = Array::new(ctx.clone())?;
    for (i, function) in table.into_iter().enumerate() {
        table_array.set(i, function)?;
    }
    tree.set("table", table_array)?;
    tree.set("signature", host.signature())?;
    // the realm of the natives, to create them again in the same context
    tree.set("realm", ctx.globals())?;
    tree.set(
        "marker",
        crate::Symbol::with_description(ctx.clone(), "rquickjs.snapshot")?,
    )?;
    Ok(tree)
}

/// The class prototypes of the natives `tree`, by class index.
fn class_prototypes<'js>(tree: &Object<'js>) -> Result<Vec<Option<Object<'js>>>> {
    tree.get::<_, Array>("classes")?
        .iter::<Object>()
        .map(|node| node?.get("prototype"))
        .collect()
}

/// The index of the registered class whose prototype (one of `prototypes`, by class index)
/// is in the prototype chain of `value`, the closest one.
fn class_of<'js>(ctx: &Ctx<'js>, prototypes: &[usize], value: &Value<'js>) -> Option<usize> {
    let mut current = value.clone();
    // a prototype chain is not circular, the bound only stops corrupted heaps
    for _ in 0..4096 {
        if unsafe { qjs::JS_IsProxy(current.as_js_value()) } != 0 {
            return None;
        }
        let proto = unsafe {
            Value::from_js_value(
                ctx.clone(),
                qjs::JS_GetPrototype(ctx.as_ptr(), current.as_js_value()),
            )
        };
        if !proto.is_object() {
            return None;
        }
        let ptr = object_ptr(&proto);
        if let Some(index) = prototypes.iter().position(|&p| p == ptr) {
            return Some(index);
        }
        current = proto;
    }
    None
}

/// The state of [`write`] given to the `save_opaque` hook.
struct WriteState<'a> {
    host: &'a SnapshotHost,
    /// Rust function (object pointer) to index in the natives table.
    functions: BTreeMap<usize, i32>,
    /// Class prototype pointers (0 if none), by class index.
    prototypes: Vec<usize>,
    /// The symbol keying tagged records (kept alive by the natives).
    marker: qjs::JSValue,
}

/// The kind of a saved Rust object in a tagged record.
const FUNCTION_TAG: i32 = -1;

impl WriteState<'_> {
    /// The saved Rust data of `obj`: the index of a native function, or the value saved by
    /// [`Persist::save`]. The reader identifies the object by its prototype chain (a function
    /// has no class prototype in it, an instance the prototype of its class); otherwise (no
    /// prototype, prototype changed) the data is tagged: `{ [marker]: class index or
    /// FUNCTION_TAG, value }`. The common case stays a primitive, which keeps deltas small.
    fn save<'js>(&self, ctx: &Ctx<'js>, obj: qjs::JSValue) -> Result<Value<'js>> {
        let Some(cell) = (unsafe { class_cell(obj, qjs::JS_GetClassID(obj)) }) else {
            return Ok(Value::new_undefined(ctx.clone()));
        };
        let v_table = unsafe { cell.as_ref().v_table };
        let value = unsafe { Value::from_js_value_const(ctx.clone(), obj) };
        let class = class_of(ctx, &self.prototypes, &value);
        let (tag, saved, natural) = if v_table.is_of_class::<RustFunction>() {
            let index = *self.functions.get(&object_ptr(&value)).ok_or_else(|| {
                Exception::throw_type(
                    ctx,
                    "a Rust function which is not one of the natives of the SnapshotHost cannot \
                     be saved",
                )
            })?;
            (
                FUNCTION_TAG,
                Value::new_int(ctx.clone(), index),
                class.is_none(),
            )
        } else {
            let index = self.host.class_index(v_table.id()).ok_or_else(|| {
                Exception::throw_type(
                    ctx,
                    "an instance of a Rust class which is not registered with \
                     SnapshotHost::class cannot be saved",
                )
            })?;
            let saved = unsafe { (self.host.0.classes[index].save)(ctx, cell)? };
            (index as i32, saved, class == Some(index))
        };
        if natural {
            return Ok(saved);
        }
        let record = Object::new(ctx.clone())?;
        let marker = unsafe { Value::from_js_value_const(ctx.clone(), self.marker) };
        record.set(marker, tag)?;
        record.set("value", saved)?;
        Ok(record.into_value())
    }
}

/// The class index (or [`FUNCTION_TAG`]) and the saved data of a tagged record.
fn tagged_record<'js>(
    ctx: &Ctx<'js>,
    marker: &Value<'js>,
    saved: &Value<'js>,
) -> Result<Option<(i32, Value<'js>)>> {
    let Some(record) = saved.as_object() else {
        return Ok(None);
    };
    let marker = crate::Atom::from_value(ctx.clone(), marker)?;
    // own data properties only: no JavaScript code runs
    let mut desc = MaybeUninit::<qjs::JSPropertyDescriptor>::uninit();
    let found = unsafe {
        qjs::JS_GetOwnProperty(
            ctx.as_ptr(),
            desc.as_mut_ptr(),
            record.as_js_value(),
            marker.atom,
        )
    };
    if found <= 0 {
        return Ok(None);
    }
    let desc = unsafe { desc.assume_init() };
    let (tag, getter, setter) = unsafe {
        (
            Value::from_js_value(ctx.clone(), desc.value),
            Value::from_js_value(ctx.clone(), desc.getter),
            Value::from_js_value(ctx.clone(), desc.setter),
        )
    };
    drop((getter, setter));
    let tag = tag
        .as_int()
        .ok_or_else(|| Error::Snapshot("invalid saved Rust object".into()))?;
    Ok(Some((tag, record.get("value")?)))
}

unsafe extern "C" fn save_opaque(
    ctx: *mut qjs::JSContext,
    obj: qjs::JSValue,
    opaque: *mut c_void,
) -> qjs::JSValue {
    let ctx = Ctx::from_ptr(ctx);
    let state = &*(opaque as *const WriteState);
    ctx.handle_panic(AssertUnwindSafe(|| match state.save(&ctx, obj) {
        Ok(value) => value.into_js_value(),
        Err(error) => error.throw(&ctx),
    }))
}

/// Implements [`Ctx::write_snapshot`].
pub(crate) fn write<'js>(
    ctx: &Ctx<'js>,
    host: &SnapshotHost,
    kind: SnapshotKind,
    roots: &[Value<'js>],
) -> Result<Vec<u8>> {
    let natives = host.natives(ctx)?;
    let mut functions = BTreeMap::new();
    for (index, function) in natives
        .tree
        .get::<_, Array>("table")?
        .iter::<Value>()
        .enumerate()
    {
        functions.insert(object_ptr(&function?), index as i32);
    }
    let prototypes = class_prototypes(&natives.tree)?
        .iter()
        .map(|p| p.as_ref().map_or(0, |p| object_ptr(p.as_value())))
        .collect();
    let marker = natives.tree.get::<_, Value>("marker")?;
    let state = WriteState {
        host,
        functions,
        prototypes,
        marker: marker.as_js_value(),
    };

    let mut raw_roots: Vec<qjs::JSValue> = core::iter::once(natives.tree.as_js_value())
        .chain(roots.iter().map(Value::as_js_value))
        .collect();
    let pointers = native_pointers();
    let mut c_host = c_host(&pointers, &state as *const WriteState as *mut c_void);
    c_host.save_opaque = Some(save_opaque);
    let flags = match kind {
        SnapshotKind::Full => 0,
        SnapshotKind::Base => qjs::JS_SNAPSHOT_BASE,
        SnapshotKind::Delta => qjs::JS_SNAPSHOT_DELTA,
    };
    unsafe {
        let mut size: qjs::size_t = 0;
        let buf = qjs::JS_WriteSnapshot(
            ctx.as_ptr(),
            &mut size,
            &c_host,
            raw_roots.as_mut_ptr(),
            raw_roots.len() as c_int,
            flags as c_int,
        );
        if buf.is_null() {
            return Err(ctx.raise_exception());
        }
        let snapshot = slice::from_raw_parts(buf, size as usize).to_vec();
        qjs::js_free(ctx.as_ptr(), buf.cast());
        Ok(snapshot)
    }
}

type Records = Vec<(qjs::JSValue, qjs::JSValue)>;

/// The `load_opaque` hook: the Rust data is attached by [`attach`] once the snapshot is read.
unsafe extern "C" fn load_opaque(
    ctx: *mut qjs::JSContext,
    obj: qjs::JSValue,
    saved: qjs::JSValue,
    opaque: *mut c_void,
) -> c_int {
    let records = &mut *(opaque as *mut Records);
    records.push((qjs::JS_DupValue(ctx, obj), qjs::JS_DupValue(ctx, saved)));
    0
}

/// Implements [`Runtime::read_snapshot`].
pub(crate) fn read(
    runtime: &Runtime,
    host: &SnapshotHost,
    base: &[u8],
    delta: Option<&[u8]>,
) -> Result<Restored> {
    let guard = runtime.inner.lock();
    guard.update_stack_top();
    let rt = guard.rt.as_ptr();

    let mut records = Records::new();
    let pointers = native_pointers();
    let mut c_host = c_host(&pointers, &mut records as *mut Records as *mut c_void);
    c_host.load_opaque = Some(load_opaque);
    let (delta_ptr, delta_len) = delta.map_or((ptr::null(), 0), |d| (d.as_ptr(), d.len()));
    let mut res = MaybeUninit::<qjs::JSSnapshotResult>::zeroed();
    let mut message = [0 as c_char; 256];
    let ret = unsafe {
        qjs::JS_ReadSnapshot(
            rt,
            base.as_ptr(),
            base.len() as _,
            delta_ptr,
            delta_len as _,
            &c_host,
            res.as_mut_ptr(),
            message.as_mut_ptr(),
            message.len() as _,
        )
    };
    if ret < 0 {
        for (obj, saved) in records {
            unsafe {
                qjs::JS_FreeValueRT(rt, obj);
                qjs::JS_FreeValueRT(rt, saved);
            }
        }
        let message = unsafe { CStr::from_ptr(message.as_ptr()) };
        return Err(Error::Snapshot(message.to_string_lossy().into_owned()));
    }

    let mut res = unsafe { res.assume_init() };
    let contexts: Vec<Context> = (0..res.context_count as usize)
        .map(|i| unsafe {
            Context::from_raw(
                NonNull::new_unchecked(*res.contexts.add(i)),
                runtime.clone(),
            )
        })
        .collect();
    let raw_roots: Vec<qjs::JSValue> = (0..res.root_count as usize)
        .map(|i| unsafe { *res.roots.add(i) })
        .collect();
    unsafe { qjs::JS_FreeSnapshotResult(rt, &mut res) };

    // a snapshot is written by a context
    let first = unsafe { Ctx::from_ptr(contexts[0].as_raw().as_ptr()) };
    let mut roots = raw_roots
        .into_iter()
        .map(|v| unsafe { Value::from_js_value(first.clone(), v) });
    let natives = roots
        .next()
        .and_then(Value::into_object)
        .ok_or_else(|| Error::Snapshot("not a snapshot of an rquickjs runtime".into()))?;
    let roots = roots.map(|v| Persistent::save(&first, v)).collect();
    // the context which created the natives
    let realm: Object = natives.get("realm")?;
    let realm = contexts
        .iter()
        .map(|c| unsafe { Ctx::from_ptr(c.as_raw().as_ptr()) })
        .find(|ctx| ctx.globals() == realm)
        .ok_or_else(|| Error::Snapshot("the context of the natives is missing".into()))?;
    let records = records
        .into_iter()
        .map(|(obj, saved)| unsafe {
            (
                Value::from_js_value(realm.clone(), obj),
                Value::from_js_value(realm.clone(), saved),
            )
        })
        .collect();
    attach(host, &realm, rt, natives, records)?;
    Ok(Restored { contexts, roots })
}

/// Attaches the Rust data of the objects read by [`read`]: the Rust functions take the code
/// of the natives created again by `host` in their context `ctx`, the class instances are
/// loaded with [`Persist::load`].
fn attach<'js>(
    host: &SnapshotHost,
    ctx: &Ctx<'js>,
    rt: *mut qjs::JSRuntime,
    tree: Object<'js>,
    records: Vec<(Value<'js>, Value<'js>)>,
) -> Result<()> {
    let opaque = unsafe { ctx.get_opaque() };
    if tree.get::<_, StdString>("signature")? != host.signature() {
        return Err(Error::Snapshot(
            "the natives of the SnapshotHost differ from the ones of the snapshot".into(),
        ));
    }
    let prototypes = class_prototypes(&tree)?;
    let prototype_ptrs: Vec<usize> = prototypes
        .iter()
        .map(|p| p.as_ref().map_or(0, |p| object_ptr(p.as_value())))
        .collect();
    let table_len = tree.get::<_, Array>("table")?.len();
    let marker: Value = tree.get("marker")?;

    // The fresh natives get fresh class prototypes, then the code of their Rust functions
    // moves to the saved ones. They are garbage afterwards.
    opaque.reset_prototypes(core::iter::empty());
    let fresh = build_natives(host, ctx)?;
    let fresh_table = fresh.get::<_, Array>("table")?;
    if fresh_table.len() != table_len {
        return Err(Error::Snapshot(
            "the natives of the SnapshotHost differ from the ones of the snapshot".into(),
        ));
    }

    let callable_id = Opaque::callable_id();
    let mut instances = Vec::new();
    for (obj, saved) in records {
        let raw = obj.as_js_value();
        let (tag, saved) = match tagged_record(ctx, &marker, &saved)? {
            Some(record) => record,
            None => match class_of(ctx, &prototype_ptrs, &obj) {
                Some(index) => (index as i32, saved),
                None if unsafe { qjs::JS_GetClassID(raw) } == callable_id => (FUNCTION_TAG, saved),
                None => {
                    return Err(Error::Snapshot(
                        "a Rust object of the snapshot has no registered class".into(),
                    ))
                }
            },
        };
        if tag != FUNCTION_TAG {
            instances.push((obj, tag, saved));
            continue;
        }
        let function: Value = saved
            .as_int()
            .and_then(|i| fresh_table.get(i as usize).ok())
            .ok_or_else(|| Error::Snapshot("invalid native function".into()))?;
        let cell = unsafe { class_cell(function.as_js_value(), callable_id) }
            .ok_or_else(|| Error::Snapshot("invalid native function".into()))?;
        unsafe {
            qjs::JS_SetOpaque(function.as_js_value(), ptr::null_mut());
            qjs::JS_SetOpaque(raw, cell.as_ptr().cast());
        }
    }

    // the loaders create instances with the saved prototypes
    opaque.reset_prototypes(
        host.0
            .classes
            .iter()
            .zip(prototypes)
            .filter_map(|(class, proto)| Some((class.id, proto?))),
    );
    for (obj, tag, saved) in instances {
        let class = usize::try_from(tag)
            .ok()
            .and_then(|i| host.0.classes.get(i))
            .ok_or_else(|| Error::Snapshot("invalid saved Rust object".into()))?;
        let cell = (class.load)(ctx, saved)?;
        unsafe { qjs::JS_SetOpaque(obj.as_js_value(), cell.as_ptr().cast()) };
    }
    opaque.set_snapshot_natives(Some((tree, host.clone())));
    drop(fresh_table);
    drop(fresh);
    unsafe { qjs::JS_RunGC(rt) };
    Ok(())
}

/// Records `request` and suspends the call of the builtin: the host carries the request out
/// in [`complete_builtin`] (the snapshot) or returns it (the restoration).
fn suspend_builtin<'js>(ctx: &Ctx<'js>, request: BuiltinRequest) -> Result<Value<'js>> {
    unsafe {
        let opaque = ctx.get_opaque();
        opaque.set_builtin_request(Some(request));
        qjs::JS_SuspendCall(ctx.as_ptr());
        if qjs::JS_IsSuspendException(ctx.as_ptr()) != 0 {
            Err(Error::Suspended)
        } else {
            opaque.set_builtin_request(None);
            Err(ctx.raise_exception())
        }
    }
}

fn snapshot_builtin<'js>(ctx: Ctx<'js>, path: Opt<StdString>) -> Result<Value<'js>> {
    suspend_builtin(&ctx, BuiltinRequest::Snapshot(path.0))
}

fn restore_builtin<'js>(
    ctx: Ctx<'js>,
    state: Value<'js>,
    value: Opt<Value<'js>>,
) -> Result<Value<'js>> {
    let state = if let Some(path) = state.as_string() {
        read_file(&path.to_string()?)?
    } else if let Some(array) = state
        .as_object()
        .and_then(|o| TypedArray::<u8>::from_object(o.clone()).ok())
    {
        // SAFETY: the bytes are copied before any JavaScript code runs
        unsafe { array.as_bytes() }
            .ok_or_else(|| Exception::throw_type(&ctx, "detached state buffer"))?
            .to_vec()
    } else {
        return Err(Exception::throw_type(
            &ctx,
            "restore() expects a Uint8Array or a file path",
        ));
    };
    let value = value
        .0
        .unwrap_or_else(|| Value::new_bool(ctx.clone(), true));
    let value = unsafe {
        let mut size: qjs::size_t = 0;
        let buf = qjs::JS_WriteObject(
            ctx.as_ptr(),
            &mut size,
            value.as_js_value(),
            qjs::JS_WRITE_OBJ_REFERENCE as c_int,
        );
        if buf.is_null() {
            return Err(ctx.raise_exception());
        }
        let bytes = slice::from_raw_parts(buf, size as usize).to_vec();
        qjs::js_free(ctx.as_ptr(), buf.cast());
        bytes
    };
    suspend_builtin(&ctx, BuiltinRequest::Restore(Restore { state, value }))
}

#[cfg(feature = "std")]
fn read_file(path: &str) -> Result<Vec<u8>> {
    Ok(std::fs::read(path)?)
}

#[cfg(feature = "std")]
fn write_file(path: &str, data: &[u8]) -> Result<()> {
    Ok(std::fs::write(path, data)?)
}

#[cfg(not(feature = "std"))]
fn read_file(_path: &str) -> Result<Vec<u8>> {
    Err(Error::Snapshot(
        "files are not supported without std".into(),
    ))
}

#[cfg(not(feature = "std"))]
fn write_file(_path: &str, _data: &[u8]) -> Result<()> {
    Err(Error::Snapshot(
        "files are not supported without std".into(),
    ))
}

/// Carries out the request of the builtin whose call suspended the execution and continues
/// it. Returns the result of [`qjs::JS_ResumeWith`] ([`Error::Restore`] for a restoration).
pub(crate) fn complete_builtin(ctx: &Ctx<'_>, request: BuiltinRequest) -> Result<qjs::JSValue> {
    let path = match request {
        BuiltinRequest::Snapshot(path) => path,
        BuiltinRequest::Restore(restore) => return Err(Error::Restore(restore)),
    };
    let result = (|| {
        let host = unsafe { ctx.get_opaque().snapshot_host() }
            .ok_or_else(|| Error::Snapshot("the runtime has no natives".into()))?;
        let state = write(ctx, &host, SnapshotKind::Full, &[])?;
        match path {
            Some(path) => {
                write_file(&path, &state)?;
                Ok(Value::new_undefined(ctx.clone()))
            }
            None => TypedArray::<u8>::new(ctx.clone(), state).map(|a| a.into_value()),
        }
    })();
    Ok(unsafe {
        match result {
            Ok(value) => qjs::JS_ResumeWith(ctx.as_ptr(), value.into_js_value(), 0),
            Err(error) => {
                // the error is thrown by the snapshot() call
                error.throw(ctx);
                let error = ctx.catch();
                qjs::JS_ResumeWith(ctx.as_ptr(), error.into_js_value(), 1)
            }
        }
    })
}
