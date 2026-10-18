//! QuickJS runtime related types.

mod base;
mod exotic;
pub(crate) mod opaque;
pub(crate) mod raw;
mod userdata;

#[cfg(feature = "futures")]
mod r#async;
#[cfg(feature = "futures")]
pub(crate) mod schedular;
#[cfg(feature = "futures")]
mod spawner;
#[cfg(feature = "futures")]
pub use spawner::DriveFuture;

use alloc::{boxed::Box, sync::Arc};
pub use base::{Runtime, WeakRuntime};
use core::sync::atomic::{AtomicBool, Ordering};
pub use userdata::{UserDataError, UserDataGuard};

#[cfg(feature = "futures")]
pub(crate) use r#async::InnerRuntime;
#[cfg(feature = "futures")]
pub use r#async::{AsyncRuntime, AsyncWeakRuntime};

use crate::value::promise::PromiseHookType;
use crate::{Ctx, Value};

/// The type of the promise hook.
#[cfg(not(feature = "parallel"))]
pub type PromiseHook =
    Box<dyn for<'a> Fn(Ctx<'a>, PromiseHookType, Value<'a>, Value<'a>) + 'static>;
/// The type of the promise hook.
#[cfg(feature = "parallel")]
pub type PromiseHook =
    Box<dyn for<'a> Fn(Ctx<'a>, PromiseHookType, Value<'a>, Value<'a>) + Send + 'static>;

/// The type of the promise rejection tracker.
#[cfg(not(feature = "parallel"))]
pub type RejectionTracker = Box<dyn for<'a> Fn(Ctx<'a>, Value<'a>, Value<'a>, bool) + 'static>;
/// The type of the promise rejection tracker.
#[cfg(feature = "parallel")]
pub type RejectionTracker =
    Box<dyn for<'a> Fn(Ctx<'a>, Value<'a>, Value<'a>, bool) + Send + 'static>;

/// The type of the interrupt handler.
#[cfg(not(feature = "parallel"))]
pub type InterruptHandler = Box<dyn FnMut() -> bool + 'static>;
/// The type of the interrupt handler.
#[cfg(feature = "parallel")]
pub type InterruptHandler = Box<dyn FnMut() -> bool + Send + 'static>;

/// A struct with information about the runtimes memory usage.
pub type MemoryUsage = crate::qjs::JSMemoryUsage;

/// How the TypeScript code of a runtime compiles decorators, like tsc's `experimentalDecorators`
/// and `emitDecoratorMetadata`; set with [`Runtime::set_decorators`]. JavaScript always uses
/// standard decorators.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Decorators {
    /// Standard ECMAScript decorators.
    #[default]
    Standard,
    /// TypeScript legacy decorators (`experimentalDecorators`), including parameter decorators.
    Legacy,
    /// Legacy decorators which also define the `design:type`, `design:paramtypes` and
    /// `design:returntype` metadata of the annotations (`emitDecoratorMetadata`), through
    /// `Reflect.metadata` when it exists at run time.
    LegacyWithMetadata,
}

impl Decorators {
    /// The eval flags added to the flags of TypeScript source.
    pub(crate) fn eval_flags(self) -> i32 {
        let flags = match self {
            Decorators::Standard => 0,
            Decorators::Legacy => crate::qjs::JS_EVAL_FLAG_TS_LEGACY_DECORATORS,
            Decorators::LegacyWithMetadata => {
                crate::qjs::JS_EVAL_FLAG_TS_LEGACY_DECORATORS
                    | crate::qjs::JS_EVAL_FLAG_TS_DECORATOR_METADATA
            }
        };
        flags as i32
    }
}

/// Asks a runtime to suspend its running JavaScript.
///
/// The suspension happens at the next interrupt check (every few thousand branches) where the
/// whole call stack is suspendable: plain JavaScript functions called by the host, with no Rust
/// callback, getter, `forEach` callback, async function or generator frame in between (see
/// `JS_RequestSuspend()` in `quickjs.h`; otherwise the request waits for such a point). The call
/// which started the execution then returns [`Error::Suspended`](crate::Error::Suspended) and
/// [`Ctx::resume`](crate::Ctx::resume) continues it, in the same runtime or in a runtime
/// restored from a snapshot.
///
/// Obtained with [`Runtime::suspender`]. Clones share the request; they can be used from any
/// thread and from the interrupt handler (e.g. to implement time slices).
#[derive(Clone, Debug)]
pub struct Suspender(Arc<AtomicBool>);

impl Suspender {
    /// Requests the suspension of the running (or next) JavaScript execution.
    pub fn request(&self) {
        self.0.store(true, Ordering::Release);
    }
}
