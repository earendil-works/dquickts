use crate::{
    class::{
        ffi::{ClassCell, VTable},
        ClassKind, JsCell, JsClass, Readable, Trace, Tracer,
    },
    qjs,
    value::function::Params,
    Ctx, Function, JsLifetime, Object, Result, Value,
};

use alloc::boxed::Box;
use core::{marker::PhantomData, panic::AssertUnwindSafe, ptr::NonNull};

use super::{Constructor, IntoJsFunc};

pub unsafe extern "C" fn defer_call_job(
    ctx: *mut qjs::JSContext,
    argc: qjs::c_int,
    argv: *mut qjs::JSValue,
) -> qjs::JSValue {
    let func = argv.offset((argc - 1) as _).read_unaligned();
    let this = argv.offset((argc - 2) as _).read_unaligned();
    let argc = argc - 2;
    qjs::JS_Call(ctx, func, this, argc, argv)
}

/// A trait for dynamic callbacks to Rust.
pub trait RustFunc<'js> {
    /// Call the actual function with a given set of parameters and return a function.
    fn call<'a>(&self, params: Params<'a, 'js>) -> Result<Value<'js>>;
}

impl<'js, F> RustFunc<'js> for F
where
    for<'a> F: Fn(Params<'a, 'js>) -> Result<Value<'js>>,
{
    fn call<'a>(&self, params: Params<'a, 'js>) -> Result<Value<'js>> {
        (self)(params)
    }
}

/// The class used for wrapping closures, rquickjs implements callbacks by creating an instances of
/// this class.
pub struct RustFunction<'js>(pub Box<dyn RustFunc<'js> + 'js>);

/// The Rust function of a [`Function::new`] function, `F` called with the parameter types `P`.
///
/// Its cell has a v-table of its own, with a call function which knows `F` and `P`: a call
/// from JavaScript neither goes through the dynamic dispatch of the [`RustFunction`] nor
/// wraps the result in a [`Value`]. To everything else it is a `RustFunction`, which calls it
/// through `call`.
pub(crate) struct JsFn<'js, F> {
    f: F,
    /// `F::call` with the parameter check, for the dynamic dispatch of the `RustFunction`.
    call: for<'a> fn(&F, Params<'a, 'js>) -> Result<Value<'js>>,
}

impl<'js, F> JsFn<'js, F> {
    /// The cell of the class object of a function of `f`.
    pub(crate) fn cell<P>(f: F) -> ClassCell<JsCell<'js, RustFunction<'js>>>
    where
        F: IntoJsFunc<'js, P> + 'js,
    {
        let func = JsFn {
            f,
            call: Self::call_value::<P>,
        };
        ClassCell {
            v_table: &<CallMarker<F, P> as HasCall>::V_TABLE,
            data: JsCell::new(RustFunction(Box::new(func))),
        }
    }

    #[inline]
    fn call_raw<P>(&self, params: Params<'_, 'js>) -> Result<qjs::JSValue>
    where
        F: IntoJsFunc<'js, P>,
    {
        params.check_params(<F as IntoJsFunc<'js, P>>::param_requirements())?;
        <F as IntoJsFunc<'js, P>>::call_raw(&self.f, params)
    }

    /// Through `call_raw` (which is instantiated anyway), this is only reached by a call of the
    /// `RustFunction` from Rust.
    fn call_value<P>(f: &F, params: Params<'_, 'js>) -> Result<Value<'js>>
    where
        F: IntoJsFunc<'js, P>,
    {
        let ctx = params.ctx().clone();
        params.check_params(<F as IntoJsFunc<'js, P>>::param_requirements())?;
        let value = <F as IntoJsFunc<'js, P>>::call_raw(f, params)?;
        // SAFETY: an owned value of the context of the call
        Ok(unsafe { Value::from_js_value(ctx, value) })
    }

    /// The call function of the v-table of a `JsFn<F>` called with `P`.
    ///
    /// # Safety
    /// The cell must be the cell of a `JsFn<F>` made by [`JsFn::cell`] with `P`.
    unsafe fn call_ffi<P>(
        this_ptr: NonNull<ClassCell<()>>,
        ctx: *mut qjs::JSContext,
        function: qjs::JSValue,
        this: qjs::JSValue,
        argc: qjs::c_int,
        argv: *mut qjs::JSValue,
        flags: qjs::c_int,
    ) -> qjs::JSValue
    where
        F: IntoJsFunc<'js, P>,
    {
        let cell = this_ptr.cast::<ClassCell<JsCell<'js, RustFunction<'js>>>>();
        // `RustFunction` is `Readable`: its cell is the function itself, which is never borrowed
        // mutably.
        let func: &(dyn RustFunc<'js> + 'js) = &*cell.as_ref().data.cell.0;
        // SAFETY: the v-table of the cell was made for the `JsFn<F>` in its box (`JsFn::cell`),
        // they move together
        let func = &*(func as *const (dyn RustFunc<'js> + 'js)).cast::<Self>();

        let params = Params::from_ffi_class(ctx, function, this, argc, argv, flags);
        // An uncounted context for handling a panic or throwing the error after `params` has been
        // consumed: the engine keeps the context alive for the whole call.
        let ctx = Ctx::from_ptr_uncounted(ctx);
        ctx.handle_panic(AssertUnwindSafe(|| match func.call_raw::<P>(params) {
            Ok(value) => value,
            Err(error) => error.throw(&ctx),
        }))
    }
}

impl<'js, F> RustFunc<'js> for JsFn<'js, F> {
    fn call<'a>(&self, params: Params<'a, 'js>) -> Result<Value<'js>> {
        (self.call)(&self.f, params)
    }
}

/// The v-tables of the [`JsFn`]s, by function and parameter types.
trait HasCall {
    const V_TABLE: VTable;
}

struct CallMarker<F, P>(PhantomData<(F, P)>);

impl<'js, F, P> HasCall for CallMarker<F, P>
where
    F: IntoJsFunc<'js, P> + 'js,
{
    const V_TABLE: VTable = VTable::rust_function(JsFn::<'js, F>::call_ffi::<P>);
}

unsafe impl<'js> JsLifetime<'js> for RustFunction<'js> {
    type Changed<'to> = RustFunction<'to>;
}

impl<'js> Trace<'js> for RustFunction<'js> {
    fn trace<'a>(&self, _tracer: Tracer<'a, 'js>) {}
}

impl<'js> JsClass<'js> for RustFunction<'js> {
    const NAME: &'static str = "RustFunction";

    type Mutable = Readable;

    const KIND: ClassKind = ClassKind::Callable;

    fn prototype(ctx: &Ctx<'js>) -> Result<Option<Object<'js>>> {
        Ok(Some(Function::prototype(ctx.clone())))
    }

    fn constructor(_ctx: &Ctx<'js>) -> Result<Option<Constructor<'js>>> {
        Ok(None)
    }

    fn call<'a>(this: &JsCell<'js, Self>, params: Params<'a, 'js>) -> Result<Value<'js>> {
        this.borrow().0.call(params)
    }
}
