use crate::{
    class::{self, ffi::VTable, JsClass},
    qjs, Ctx, Error, JsLifetime, Object, Value,
};

use super::{
    exotic::ExoticMethodsHolder,
    userdata::{UserDataGuard, UserDataMap},
    InterruptHandler, PromiseHook, PromiseHookType, RejectionTracker, UserDataError,
};
use super::{Decorators, Suspender};
use crate::snapshot::{BuiltinRequest, SnapshotHost};
use alloc::{boxed::Box, sync::Arc};
use core::sync::atomic::{AtomicBool, Ordering};
use core::{
    any::{Any, TypeId},
    cell::{Cell, UnsafeCell},
    marker::PhantomData,
    ptr,
};

#[cfg(feature = "std")]
use std::collections::{hash_map::Entry, HashMap};

#[cfg(not(feature = "std"))]
use hashbrown::{hash_map::Entry, HashMap};

#[cfg(feature = "futures")]
use super::{schedular::SchedularPoll, spawner::Spawner};

#[cfg(feature = "futures")]
use core::{
    future::Future,
    task::{Context, Waker},
};

/// Opaque book keeping data for Rust.
pub(crate) struct Opaque<'js> {
    /// Used to carry a panic if a callback triggered one.
    panic: Cell<Option<Box<dyn Any + Send + 'static>>>,

    /// The user provided promise hook, if any.
    promise_hook: UnsafeCell<Option<PromiseHook>>,

    /// The user provided rejection tracker, if any.
    rejection_tracker: UnsafeCell<Option<RejectionTracker>>,

    /// The user provided interrupt handler, if any.
    interrupt_handler: UnsafeCell<Option<InterruptHandler>>,

    /// Set by [`Suspender::request`], polled by the interrupt handler trampoline.
    suspend_request: Arc<AtomicBool>,

    /// The natives of the [`SnapshotHost`] of this runtime, built on first use or restored from
    /// a snapshot.
    snapshot_natives: UnsafeCell<Option<(Object<'js>, SnapshotHost)>>,

    /// The request of the snapshot builtin whose call is suspending the execution.
    builtin_request: UnsafeCell<Option<BuiltinRequest>>,

    /// The value the next [`Ctx::resume`](crate::Ctx::resume) gives to the suspended call
    /// (see [`Restore::apply`](crate::snapshot::Restore::apply)).
    resume_value: UnsafeCell<Option<Value<'js>>>,

    /// How TypeScript sources compile decorators.
    decorators: Cell<Decorators>,

    /// The class id for rust classes.
    class_id: qjs::JSClassID,
    /// The class id for rust classes which can be called.
    callable_class_id: qjs::JSClassID,
    /// The class id for exotic classes
    exotic_class_id: qjs::JSClassID,

    prototypes: UnsafeCell<HashMap<TypeId, Option<Object<'js>>>>,

    userdata: UserDataMap,

    #[cfg(feature = "futures")]
    spawner: Option<UnsafeCell<Spawner>>,

    exotic_methods: ExoticMethodsHolder,

    _marker: PhantomData<&'js ()>,
}

impl<'js> Opaque<'js> {
    pub fn new() -> Self {
        Opaque {
            panic: Cell::new(None),

            promise_hook: UnsafeCell::new(None),

            rejection_tracker: UnsafeCell::new(None),

            interrupt_handler: UnsafeCell::new(None),

            suspend_request: Arc::new(AtomicBool::new(false)),

            snapshot_natives: UnsafeCell::new(None),

            builtin_request: UnsafeCell::new(None),

            resume_value: UnsafeCell::new(None),

            decorators: Cell::new(Decorators::Standard),

            class_id: qjs::JS_INVALID_CLASS_ID,
            callable_class_id: qjs::JS_INVALID_CLASS_ID,
            exotic_class_id: qjs::JS_INVALID_CLASS_ID,

            prototypes: UnsafeCell::new(HashMap::new()),

            userdata: UserDataMap::default(),

            _marker: PhantomData,

            #[cfg(feature = "futures")]
            spawner: None,

            exotic_methods: ExoticMethodsHolder::new(),
        }
    }

    #[cfg(feature = "futures")]
    pub fn with_spawner() -> Self {
        let mut this = Opaque::new();
        this.spawner = Some(UnsafeCell::new(Spawner::new()));
        this
    }

    pub unsafe fn initialize(&mut self, rt: *mut qjs::JSRuntime) -> Result<(), Error> {
        // Class ids are process wide in QuickJS: allocate them once so that
        // every runtime uses the same ids (JS_NewClassID() is serialized and
        // keeps an already allocated id).
        static mut CLASS_ID: qjs::JSClassID = 0;
        static mut CALLABLE_CLASS_ID: qjs::JSClassID = 0;
        static mut EXOTIC_CLASS_ID: qjs::JSClassID = 0;
        self.class_id = qjs::JS_NewClassID(&raw mut CLASS_ID);
        self.callable_class_id = qjs::JS_NewClassID(&raw mut CALLABLE_CLASS_ID);
        self.exotic_class_id = qjs::JS_NewClassID(&raw mut EXOTIC_CLASS_ID);

        let class_def = qjs::JSClassDef {
            class_name: c"RustClass".as_ptr().cast(),
            finalizer: Some(class::ffi::class_finalizer),
            gc_mark: Some(class::ffi::class_trace),
            call: None,
            exotic: ptr::null_mut(),
        };

        if 0 != qjs::JS_NewClass(rt, self.class_id, &class_def) {
            return Err(Error::Unknown);
        }

        let class_def = qjs::JSClassDef {
            class_name: c"RustFunction".as_ptr().cast(),
            finalizer: Some(class::ffi::callable_finalizer),
            gc_mark: Some(class::ffi::callable_trace),
            call: Some(class::ffi::call),
            exotic: ptr::null_mut(),
        };

        if 0 != qjs::JS_NewClass(rt, self.callable_class_id, &class_def) {
            return Err(Error::Unknown);
        }

        let class_def = qjs::JSClassDef {
            class_name: c"RustExotic".as_ptr().cast(),
            finalizer: Some(class::ffi::exotic_class_finalizer),
            gc_mark: Some(class::ffi::exotic_class_trace),
            call: None,
            exotic: self.exotic_methods.as_ptr(),
        };

        if 0 != qjs::JS_NewClass(rt, self.exotic_class_id, &class_def) {
            return Err(Error::Unknown);
        }

        Ok(())
    }

    pub unsafe fn from_runtime_ptr<'a>(rt: *mut qjs::JSRuntime) -> &'a Self {
        &*(qjs::JS_GetRuntimeOpaque(rt).cast::<Self>())
    }

    #[cfg(feature = "futures")]
    fn spawner(&self) -> &UnsafeCell<Spawner> {
        self.spawner
            .as_ref()
            .expect("tried to use async function in non async runtime")
    }

    #[cfg(feature = "futures")]
    pub unsafe fn push<F>(&self, f: F)
    where
        F: Future<Output = ()>,
    {
        (*self.spawner().get()).push(f)
    }

    #[cfg(feature = "futures")]
    pub fn listen(&self, wake: Waker) {
        unsafe { (*self.spawner().get()).listen(wake) };
    }

    #[cfg(feature = "futures")]
    pub fn spawner_is_empty(&self) -> bool {
        unsafe { (*self.spawner().get()).is_empty() }
    }

    #[cfg(feature = "futures")]
    pub fn poll(&self, cx: &mut Context) -> SchedularPoll {
        unsafe { (*self.spawner().get()).poll(cx) }
    }

    pub fn insert_userdata<U>(&self, data: U) -> Result<Option<Box<U>>, UserDataError<U>>
    where
        U: JsLifetime<'js>,
        U::Changed<'static>: Any,
    {
        self.userdata.insert(data)
    }

    pub fn remove_userdata<U>(&self) -> Result<Option<Box<U>>, UserDataError<()>>
    where
        U: JsLifetime<'js>,
        U::Changed<'static>: Any,
    {
        self.userdata.remove()
    }

    pub fn get_userdata<U>(&self) -> Option<UserDataGuard<U>>
    where
        U: JsLifetime<'js>,
        U::Changed<'static>: Any,
    {
        self.userdata.get()
    }

    pub fn set_promise_hook(&self, promise_hook: Option<PromiseHook>) {
        unsafe { (*self.promise_hook.get()) = promise_hook }
    }

    pub fn run_promise_hook<'a>(
        &self,
        ctx: Ctx<'a>,
        type_: PromiseHookType,
        promise: Value<'a>,
        parent: Value<'a>,
    ) {
        unsafe { (*self.promise_hook.get()).as_mut().unwrap()(ctx, type_, promise, parent) }
    }

    pub fn set_rejection_tracker(&self, tracker: Option<RejectionTracker>) {
        unsafe { (*self.rejection_tracker.get()) = tracker }
    }

    pub fn run_rejection_tracker<'a>(
        &self,
        ctx: Ctx<'a>,
        promise: Value<'a>,
        reason: Value<'a>,
        is_handled: bool,
    ) {
        unsafe {
            (*self.rejection_tracker.get()).as_mut().unwrap()(ctx, promise, reason, is_handled)
        }
    }

    pub fn set_interrupt_handler(&self, interupt: Option<InterruptHandler>) {
        unsafe { (*self.interrupt_handler.get()) = interupt }
    }

    pub fn run_interrupt_handler(&self) -> bool {
        unsafe {
            (*self.interrupt_handler.get())
                .as_mut()
                .is_some_and(|h| h())
        }
    }

    pub fn suspender(&self) -> Suspender {
        Suspender(self.suspend_request.clone())
    }

    pub fn decorators(&self) -> Decorators {
        self.decorators.get()
    }

    pub fn set_decorators(&self, decorators: Decorators) {
        self.decorators.set(decorators);
    }

    pub fn take_suspend_request(&self) -> bool {
        self.suspend_request.swap(false, Ordering::AcqRel)
    }

    pub fn snapshot_natives(&self) -> Option<Object<'js>> {
        unsafe {
            (*self.snapshot_natives.get())
                .as_ref()
                .map(|(tree, _)| tree.clone())
        }
    }

    pub fn snapshot_host(&self) -> Option<SnapshotHost> {
        unsafe {
            (*self.snapshot_natives.get())
                .as_ref()
                .map(|(_, host)| host.clone())
        }
    }

    pub fn set_snapshot_natives(&self, natives: Option<(Object<'js>, SnapshotHost)>) {
        unsafe { *self.snapshot_natives.get() = natives }
    }

    pub fn set_builtin_request(&self, request: Option<BuiltinRequest>) {
        unsafe { *self.builtin_request.get() = request }
    }

    pub fn take_builtin_request(&self) -> Option<BuiltinRequest> {
        unsafe { (*self.builtin_request.get()).take() }
    }

    pub fn set_resume_value(&self, value: Option<Value<'js>>) {
        unsafe { *self.resume_value.get() = value }
    }

    pub fn take_resume_value(&self) -> Option<Value<'js>> {
        unsafe { (*self.resume_value.get()).take() }
    }

    /// Replaces the cached prototypes of the Rust classes.
    pub fn reset_prototypes(&self, prototypes: impl IntoIterator<Item = (TypeId, Object<'js>)>) {
        unsafe {
            let map = &mut *self.prototypes.get();
            map.clear();
            map.extend(prototypes.into_iter().map(|(id, proto)| (id, Some(proto))));
        }
    }

    #[allow(dead_code)] // not used in no_std
    pub fn set_panic(&self, panic: Box<dyn Any + Send + 'static>) {
        self.panic.set(Some(panic))
    }

    pub fn take_panic(&self) -> Option<Box<dyn Any + Send + 'static>> {
        self.panic.take()
    }

    pub fn get_class_id(&self) -> qjs::JSClassID {
        self.class_id
    }

    pub fn get_callable_id(&self) -> qjs::JSClassID {
        self.callable_class_id
    }

    pub fn get_exotic_id(&self) -> qjs::JSClassID {
        self.exotic_class_id
    }

    pub fn get_or_insert_prototype<C: JsClass<'js>>(
        &self,
        ctx: &Ctx<'js>,
    ) -> Result<Option<Object<'js>>, Error> {
        unsafe {
            let vtable = VTable::get::<C>();
            let id = vtable.id();
            match (*self.prototypes.get()).entry(id) {
                Entry::Occupied(x) => Ok(x.get().clone()),
                Entry::Vacant(x) => {
                    let proto = C::prototype(ctx)?;
                    Ok(x.insert(proto).clone())
                }
            }
        }
    }

    /// Cleans up all the internal state.
    ///
    /// Called before dropping the runtime to ensure that we drop everything before freeing the
    /// runtime.
    pub fn clear(&mut self) {
        self.rejection_tracker.get_mut().take();
        self.interrupt_handler.get_mut().take();
        self.panic.take();
        self.prototypes.get_mut().clear();
        #[cfg(feature = "futures")]
        self.spawner.take();
        self.userdata.clear();
    }
}
