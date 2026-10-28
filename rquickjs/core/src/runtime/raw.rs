#![allow(dead_code, unused_imports)]
use alloc::{boxed::Box, ffi::CString};
use core::{mem, panic::AssertUnwindSafe, ptr::NonNull, result::Result as StdResult};
#[cfg(feature = "parallel")]
use std::sync::mpsc::Receiver;

use crate::allocator::{Allocator, AllocatorHolder};
#[cfg(feature = "loader")]
use crate::loader::{Loader, LoaderHolder, Resolver};
use crate::{
    qjs::{self, size_t},
    Ctx, Error, Result, Value,
};

use rquickjs_sys::JSPromiseHookType;

use super::{opaque::Opaque, InterruptHandler, PromiseHook, PromiseHookType, RejectionTracker};

// Build the flags using `#[cfg]` at compile time
const fn build_dump_flags() -> u64 {
    #[allow(unused_mut)]
    let mut flags: u64 = 0;

    #[cfg(feature = "dump-bytecode")]
    {
        flags |= (qjs::JS_DUMP_BYTECODE_FINAL
            | qjs::JS_DUMP_BYTECODE_PASS2
            | qjs::JS_DUMP_BYTECODE_PASS1) as u64;
    }

    #[cfg(feature = "dump-gc")]
    {
        flags |= qjs::JS_DUMP_GC as u64;
    }

    #[cfg(feature = "dump-gc-free")]
    {
        flags |= qjs::JS_DUMP_GC_FREE as u64;
    }

    #[cfg(feature = "dump-free")]
    {
        flags |= qjs::JS_DUMP_FREE as u64;
    }

    #[cfg(feature = "dump-leaks")]
    {
        flags |= qjs::JS_DUMP_LEAKS as u64;
    }

    #[cfg(feature = "dump-mem")]
    {
        flags |= qjs::JS_DUMP_MEM as u64;
    }

    #[cfg(feature = "dump-objects")]
    {
        flags |= qjs::JS_DUMP_OBJECTS as u64;
    }

    #[cfg(feature = "dump-atoms")]
    {
        flags |= qjs::JS_DUMP_ATOMS as u64;
    }

    #[cfg(feature = "dump-shapes")]
    {
        flags |= qjs::JS_DUMP_SHAPES as u64;
    }

    #[cfg(feature = "dump-module-resolve")]
    {
        flags |= qjs::JS_DUMP_MODULE_RESOLVE as u64;
    }

    #[cfg(feature = "dump-promise")]
    {
        flags |= qjs::JS_DUMP_PROMISE as u64;
    }

    #[cfg(feature = "dump-read-object")]
    {
        flags |= qjs::JS_DUMP_READ_OBJECT as u64;
    }

    flags
}

#[derive(Debug)]
pub(crate) struct RawRuntime {
    pub(crate) rt: NonNull<qjs::JSRuntime>,

    // To keep rt info alive for the entire duration of the lifetime of rt
    #[allow(dead_code)]
    pub info: Option<CString>,

    #[allow(dead_code)]
    pub allocator: Option<AllocatorHolder>,
    #[cfg(feature = "loader")]
    #[allow(dead_code)]
    pub loader: Option<LoaderHolder>,

    // Contexts whose `JS_FreeContext` could not be run at drop time because the
    // runtime lock was contended, parked here by `drop_context` and drained by
    // whoever next holds the lock, or by `RawRuntime::drop`. Uses a channel
    // rather than a `Mutex<Vec<_>>` so a panic elsewhere can never poison it
    // and silently strand a context; mirrors `AsyncRuntime`'s `pending_free`.
    #[cfg(feature = "parallel")]
    pub(crate) pending_free: Receiver<NonNull<qjs::JSContext>>,
}

#[cfg(feature = "parallel")]
unsafe impl Send for RawRuntime {}

impl Drop for RawRuntime {
    fn drop(&mut self) {
        self.drain_pending_free();
        unsafe {
            let ptr = qjs::JS_GetRuntimeOpaque(self.rt.as_ptr());
            let mut opaque: Box<Opaque> = Box::from_raw(ptr as *mut _);
            opaque.clear();
            qjs::JS_FreeRuntime(self.rt.as_ptr());
            mem::drop(opaque);
        }
    }
}

impl RawRuntime {
    /// Frees contexts parked by `drop_context` while the runtime lock was contended.
    ///
    /// Caller must hold the runtime lock, or otherwise have exclusive access, as during `Drop`.
    pub(crate) fn drain_pending_free(&self) {
        #[cfg(feature = "parallel")]
        while let Ok(ctx) = self.pending_free.try_recv() {
            unsafe { qjs::JS_FreeContext(ctx.as_ptr()) }
        }
    }

    pub unsafe fn new(
        opaque: Opaque<'static>,
        #[cfg(feature = "parallel")] pending_free: Receiver<NonNull<qjs::JSContext>>,
    ) -> Result<Self> {
        #[cfg(not(feature = "rust-alloc"))]
        return Self::new_base(
            opaque,
            #[cfg(feature = "parallel")]
            pending_free,
        );

        #[cfg(feature = "rust-alloc")]
        Self::new_with_allocator(
            opaque,
            crate::allocator::RustAllocator,
            #[cfg(feature = "parallel")]
            pending_free,
        )
    }

    #[allow(dead_code)]
    pub unsafe fn new_base(
        mut opaque: Opaque<'static>,
        #[cfg(feature = "parallel")] pending_free: Receiver<NonNull<qjs::JSContext>>,
    ) -> Result<Self> {
        let rt = qjs::JS_NewRuntime();

        let rt = NonNull::new(rt).ok_or(Error::Allocation)?;

        Self::add_dump_flags(rt.as_ptr());

        opaque.initialize(rt.as_ptr())?;

        let opaque = Box::into_raw(Box::new(opaque));
        unsafe { qjs::JS_SetRuntimeOpaque(rt.as_ptr(), opaque as *mut _) };
        Self::install_interrupt_handler(rt);

        Ok(RawRuntime {
            rt,
            info: None,
            allocator: None,
            #[cfg(feature = "loader")]
            loader: None,
            #[cfg(feature = "parallel")]
            pending_free,
        })
    }

    pub unsafe fn new_with_allocator<A>(
        mut opaque: Opaque<'static>,
        allocator: A,
        #[cfg(feature = "parallel")] pending_free: Receiver<NonNull<qjs::JSContext>>,
    ) -> Result<Self>
    where
        A: Allocator + 'static,
    {
        let allocator = AllocatorHolder::new(allocator);
        let functions = AllocatorHolder::functions::<A>();
        let opaque_ptr = allocator.opaque_ptr();

        let rt = qjs::JS_NewRuntime2(&functions, opaque_ptr as _);

        let rt = NonNull::new(rt).ok_or(Error::Allocation)?;

        Self::add_dump_flags(rt.as_ptr());

        opaque.initialize(rt.as_ptr())?;

        let opaque = Box::into_raw(Box::new(opaque));
        unsafe { qjs::JS_SetRuntimeOpaque(rt.as_ptr(), opaque as *mut _) };
        Self::install_interrupt_handler(rt);

        Ok(RawRuntime {
            rt,
            info: None,
            allocator: Some(allocator),
            #[cfg(feature = "loader")]
            loader: None,
            #[cfg(feature = "parallel")]
            pending_free,
        })
    }

    pub fn update_stack_top(&self) {
        #[cfg(feature = "parallel")]
        unsafe {
            qjs::JS_UpdateStackTop(self.rt.as_ptr());
        }
    }

    pub fn get_opaque<'js>(&self) -> &Opaque<'js> {
        unsafe { &*(qjs::JS_GetRuntimeOpaque(self.rt.as_ptr()) as *mut _) }
    }

    pub fn is_job_pending(&self) -> bool {
        (unsafe { qjs::JS_IsJobPending(self.rt.as_ptr()) } as i32) != 0
    }

    pub fn execute_pending_job(&mut self) -> StdResult<bool, *mut qjs::JSContext> {
        let mut ctx_ptr = mem::MaybeUninit::<*mut qjs::JSContext>::uninit();
        let result = unsafe { qjs::JS_ExecutePendingJob(self.rt.as_ptr(), ctx_ptr.as_mut_ptr()) };
        if result == 0 {
            // no jobs executed
            return Ok(false);
        }
        if result == 1 {
            // single job executed
            return Ok(true);
        }
        Err(unsafe { ctx_ptr.assume_init() })
    }

    /// Executes pending jobs in order, including the jobs they enqueue, until none is left
    /// or `max_jobs` were executed (`None`: no limit), in one call into the engine.
    ///
    /// Returns the number of executed jobs, or, if a job threw, the borrowed pointer to its
    /// context (where the exception is pending) after stopping at that job.
    pub fn execute_pending_jobs(
        &mut self,
        max_jobs: Option<usize>,
    ) -> StdResult<usize, *mut qjs::JSContext> {
        let max_jobs: core::ffi::c_int = max_jobs.map_or(-1, |n| {
            n.min(core::ffi::c_int::MAX as usize) as core::ffi::c_int
        });
        let mut ctx_ptr = core::ptr::null_mut();
        let result =
            unsafe { qjs::JS_ExecutePendingJobs(self.rt.as_ptr(), max_jobs, &mut ctx_ptr) };
        if result < 0 {
            return Err(ctx_ptr);
        }
        Ok(result as usize)
    }

    #[cfg(feature = "loader")]
    pub unsafe fn set_loader<R, L>(&mut self, resolver: R, loader: L)
    where
        R: Resolver + 'static,
        L: Loader + 'static,
    {
        let loader = LoaderHolder::new(resolver, loader);
        loader.set_to_runtime(self.rt.as_ptr());
        self.loader = Some(loader);
    }

    /// Set the info of the runtime
    pub unsafe fn set_info(&mut self, info: CString) {
        unsafe { qjs::JS_SetRuntimeInfo(self.rt.as_ptr(), info.as_ptr()) };
        self.info = Some(info);
    }

    /// Set a limit on the max amount of memory the runtime will use.
    ///
    /// Setting the limit to 0 is equivalent to unlimited memory.
    pub unsafe fn set_memory_limit(&mut self, limit: usize) {
        let limit: size_t = limit.try_into().unwrap_or(size_t::MAX);
        qjs::JS_SetMemoryLimit(self.rt.as_ptr(), limit)
    }

    /// Set a limit on the max size of stack the runtime will use.
    ///
    /// The default value is 256x1024 bytes. A `limit` of `0` disables the
    /// check. Values larger than 16 MiB are treated as `0` so QuickJS's
    /// `stack_top - stack_size` subtraction cannot underflow.
    pub unsafe fn set_max_stack_size(&mut self, limit: usize) {
        const MAX_SANE: usize = 1 << 24;
        let limit: size_t = if limit > MAX_SANE {
            0
        } else {
            limit.try_into().unwrap_or(0)
        };
        qjs::JS_SetMaxStackSize(self.rt.as_ptr(), limit);
    }

    /// Set a memory threshold for garbage collection.
    pub unsafe fn set_gc_threshold(&self, threshold: usize) {
        qjs::JS_SetGCThreshold(self.rt.as_ptr(), threshold as _);
    }

    /// Set dump flags.
    pub unsafe fn set_dump_flags(&self, flags: u64) {
        qjs::JS_SetDumpFlags(self.rt.as_ptr(), flags);
    }

    /// Manually run the garbage collection.
    ///
    /// Most of QuickJS values are reference counted and
    /// will automatically free themselves when they have no more
    /// references. The garbage collector is only for collecting
    /// cyclic references.
    pub unsafe fn run_gc(&mut self) {
        qjs::JS_RunGC(self.rt.as_ptr());
    }

    /// Get memory usage stats
    pub unsafe fn memory_usage(&mut self) -> qjs::JSMemoryUsage {
        let mut stats = mem::MaybeUninit::uninit();
        qjs::JS_ComputeMemoryUsage(self.rt.as_ptr(), stats.as_mut_ptr());
        stats.assume_init()
    }

    #[allow(clippy::unnecessary_cast)]
    pub unsafe fn set_promise_hook(&mut self, hook: Option<PromiseHook>) {
        unsafe extern "C" fn promise_hook_wrapper(
            ctx: *mut rquickjs_sys::JSContext,
            type_: JSPromiseHookType,
            promise: rquickjs_sys::JSValue,
            parent: rquickjs_sys::JSValue,
            opaque: *mut ::core::ffi::c_void,
        ) {
            let opaque = NonNull::new_unchecked(opaque).cast::<Opaque>();

            let catch_unwind = crate::util::catch_unwind(AssertUnwindSafe(move || {
                let ctx = Ctx::from_ptr(ctx);

                const INIT: u32 = qjs::JSPromiseHookType_JS_PROMISE_HOOK_INIT as u32;
                const BEFORE: u32 = qjs::JSPromiseHookType_JS_PROMISE_HOOK_BEFORE as u32;
                const AFTER: u32 = qjs::JSPromiseHookType_JS_PROMISE_HOOK_AFTER as u32;
                const RESOLVE: u32 = qjs::JSPromiseHookType_JS_PROMISE_HOOK_RESOLVE as u32;

                let rtype = match type_ as u32 {
                    INIT => PromiseHookType::Init,
                    BEFORE => PromiseHookType::Before,
                    AFTER => PromiseHookType::After,
                    RESOLVE => PromiseHookType::Resolve,
                    _ => unreachable!(),
                };

                opaque.as_ref().run_promise_hook(
                    ctx.clone(),
                    rtype,
                    Value::from_js_value_const(ctx.clone(), promise),
                    Value::from_js_value_const(ctx, parent),
                );
            }));
            match catch_unwind {
                Ok(_) => {}
                Err(panic) => {
                    opaque.as_ref().set_panic(panic);
                }
            }
        }

        qjs::JS_SetPromiseHook(
            self.rt.as_ptr(),
            hook.as_ref().map(|_| {
                promise_hook_wrapper
                    as unsafe extern "C" fn(
                        *mut rquickjs_sys::JSContext,
                        JSPromiseHookType,
                        rquickjs_sys::JSValue,
                        rquickjs_sys::JSValue,
                        *mut core::ffi::c_void,
                    )
            }),
            qjs::JS_GetRuntimeOpaque(self.rt.as_ptr()),
        );
        self.get_opaque().set_promise_hook(hook);
    }

    pub unsafe fn set_host_promise_rejection_tracker(&mut self, tracker: Option<RejectionTracker>) {
        unsafe extern "C" fn rejection_tracker_wrapper(
            ctx: *mut rquickjs_sys::JSContext,
            promise: rquickjs_sys::JSValue,
            reason: rquickjs_sys::JSValue,
            is_handled: qjs::c_int,
            opaque: *mut ::core::ffi::c_void,
        ) {
            let opaque = NonNull::new_unchecked(opaque).cast::<Opaque>();

            let catch_unwind = crate::util::catch_unwind(AssertUnwindSafe(move || {
                let ctx = Ctx::from_ptr(ctx);

                opaque.as_ref().run_rejection_tracker(
                    ctx.clone(),
                    Value::from_js_value_const(ctx.clone(), promise),
                    Value::from_js_value_const(ctx, reason),
                    is_handled != 0,
                );
            }));
            match catch_unwind {
                Ok(_) => {}
                Err(panic) => {
                    opaque.as_ref().set_panic(panic);
                }
            }
        }
        qjs::JS_SetHostPromiseRejectionTracker(
            self.rt.as_ptr(),
            tracker.as_ref().map(|_| rejection_tracker_wrapper as _),
            qjs::JS_GetRuntimeOpaque(self.rt.as_ptr()),
        );
        self.get_opaque().set_rejection_tracker(tracker);
    }

    /// Set a closure which is regularly called by the engine when it is executing code.
    /// If the provided closure returns `true` the interpreter will raise and uncatchable
    /// exception and return control flow to the caller.
    pub unsafe fn set_interrupt_handler(&mut self, handler: Option<InterruptHandler>) {
        self.get_opaque().set_interrupt_handler(handler);
    }

    /// TRUE if an execution is suspended (see [`Suspender`](super::Suspender)).
    pub fn is_suspended(&self) -> bool {
        unsafe { qjs::JS_IsSuspended(self.rt.as_ptr()) != 0 }
    }

    /// Installs the C interrupt handler (once the opaque is set): it runs the user interrupt
    /// handler, then forwards the suspension requests of the
    /// [`Suspender`](super::Suspender)s to the engine.
    unsafe fn install_interrupt_handler(rt: NonNull<qjs::JSRuntime>) {
        unsafe extern "C" fn interrupt_handler_trampoline(
            rt: *mut qjs::JSRuntime,
            opaque: *mut ::core::ffi::c_void,
        ) -> ::core::ffi::c_int {
            // This should be safe as the value is set below to a non-null pointer.
            let opaque = NonNull::new_unchecked(opaque).cast::<Opaque>();

            let should_interrupt = {
                let catch_unwind = crate::util::catch_unwind(AssertUnwindSafe(move || {
                    opaque.as_ref().run_interrupt_handler()
                }));
                match catch_unwind {
                    Ok(should_interrupt) => should_interrupt,
                    Err(panic) => {
                        opaque.as_ref().set_panic(panic);
                        // Returning true here will cause the interpreter to raise an un-catchable exception.
                        // The Rust code that is running the interpreter will see that exception and continue
                        // the panic handling. See crate::result::{handle_exception, handle_panic} for details.
                        true
                    }
                }
            };
            if opaque.as_ref().take_suspend_request() {
                qjs::JS_RequestSuspend(rt);
            }

            should_interrupt as _
        }

        qjs::JS_SetInterruptHandler(
            rt.as_ptr(),
            Some(interrupt_handler_trampoline),
            qjs::JS_GetRuntimeOpaque(rt.as_ptr()),
        );
    }

    /// Selects the dumps of the enabled `dump-*` features.
    fn add_dump_flags(rt: *mut qjs::JSRuntime) {
        unsafe {
            qjs::JS_SetDumpFlags(rt, build_dump_flags());
        }
    }
}

#[cfg(test)]
mod test {
    use std::sync::{Arc, Mutex};

    use crate::{qjs, Context, Runtime};

    fn dump_flags(rt: &Runtime) -> u64 {
        unsafe { qjs::JS_GetDumpFlags(rt.inner.lock().rt.as_ptr()) }
    }

    /// The `dump-*` features select the dumps of every new runtime (the engine
    /// keeps no dump flags without them).
    #[test]
    fn dump_flags_of_features() {
        let rt = Runtime::new().unwrap();
        assert_eq!(dump_flags(&rt), super::build_dump_flags());
        #[cfg(feature = "dump-gc")]
        assert_ne!(dump_flags(&rt) & qjs::JS_DUMP_GC as u64, 0);
        #[cfg(feature = "dump-bytecode")]
        assert_ne!(dump_flags(&rt) & qjs::JS_DUMP_BYTECODE_FINAL as u64, 0);
        // the dumps print while the code runs
        let context = Context::full(&rt).unwrap();
        context.with(|ctx| {
            assert_eq!(ctx.eval::<i32, _>("1 + 2").unwrap(), 3);
        });
        rt.run_gc();
    }

    #[test]
    fn set_dump_flags() {
        let rt = Runtime::new().unwrap();
        let flags = (qjs::JS_DUMP_GC | qjs::JS_DUMP_PROMISE) as u64;
        rt.set_dump_flags(flags);
        // the dumps are only compiled in with a `dump-*` feature
        let dumps_enabled = super::build_dump_flags() != 0;
        assert_eq!(dump_flags(&rt), if dumps_enabled { flags } else { 0 });
        rt.set_dump_flags(0);
        assert_eq!(dump_flags(&rt), 0);
    }

    #[test]
    fn promise_rejection_handler() {
        let counter = Arc::new(Mutex::new(0));
        let rt = Runtime::new().unwrap();
        {
            let counter = counter.clone();
            rt.set_host_promise_rejection_tracker(Some(Box::new(move |_, _, _, is_handled| {
                if !is_handled {
                    let mut c = counter.lock().unwrap();
                    *c += 1;
                }
            })));
        }
        let context = Context::full(&rt).unwrap();
        context.with(|ctx| {
            let _: Result<(), _> = ctx.eval(
                r#"
                const x = async () => {
                    throw new Error("Uncaught")
                }
                x()
                throw new Error("Caught")
            "#,
            );
        });
        assert_eq!(*counter.lock().unwrap(), 1);
    }

    #[test]
    fn promise_hook() {
        use crate::value::promise::PromiseHookType;

        let events = Arc::new(Mutex::new(Vec::new()));
        let rt = Runtime::new().unwrap();
        {
            let events = events.clone();
            rt.set_promise_hook(Some(Box::new(move |_, kind, _, parent| {
                events.lock().unwrap().push((kind, parent.is_undefined()));
            })));
        }
        let context = Context::full(&rt).unwrap();
        context.with(|ctx| {
            let _: () = ctx.eval("Promise.resolve(1).then(() => {})").unwrap();
        });
        while rt.is_job_pending() {
            rt.execute_pending_job().unwrap();
        }
        let events = events.lock().unwrap();
        // the promise of then() has a parent
        assert!(events.contains(&(PromiseHookType::Init, true)));
        assert!(events.contains(&(PromiseHookType::Init, false)));
        assert!(events.contains(&(PromiseHookType::Resolve, true)));
    }
}
