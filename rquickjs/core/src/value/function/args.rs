use crate::{
    function::{Flat, Opt, Rest, This},
    qjs, Ctx, FromJs, Function, IntoJs, Result, Value,
};
use alloc::vec::Vec;
use core::{mem, ptr::NonNull};

use super::{ffi::defer_call_job, Constructor};

const ARGS_ON_STACK: usize = 4;

pub enum ArgsSlice {
    Stack {
        slice: [qjs::JSValue; ARGS_ON_STACK],
        offset: u8,
    },
    Heap(Vec<qjs::JSValue>),
}

/// Argument input for a functions
///
/// Arguments on the Rust side for calling into the JavaScript context.
pub struct Args<'js> {
    /// Only `None` after a call moved the reference to its result, see `Args::into_result`.
    ctx: Option<Ctx<'js>>,
    raw_ctx: NonNull<qjs::JSContext>,
    pub(crate) this: qjs::JSValue,
    pub(crate) args: ArgsSlice,
}

impl<'js> Args<'js> {
    /// Returns a new args with space for a give number of arguments
    #[inline]
    pub fn new(ctx: Ctx<'js>, args: usize) -> Args<'js> {
        Args {
            raw_ctx: ctx.as_raw(),
            ctx: Some(ctx),
            this: qjs::JS_UNDEFINED,
            args: if args <= ARGS_ON_STACK {
                ArgsSlice::Stack {
                    slice: [qjs::JS_UNDEFINED; ARGS_ON_STACK],
                    offset: 0,
                }
            } else {
                ArgsSlice::Heap(Vec::with_capacity(args))
            },
        }
    }

    /// Returns a new args with space for any number of arguments
    #[inline]
    pub fn new_unsized(ctx: Ctx<'js>) -> Args<'js> {
        Args {
            raw_ctx: ctx.as_raw(),
            ctx: Some(ctx),
            this: qjs::JS_UNDEFINED,
            args: ArgsSlice::Heap(Vec::new()),
        }
    }

    /// Returns the context associated with these arguments.
    #[inline]
    pub fn ctx(&self) -> &Ctx<'js> {
        // SAFETY: the context is only taken by `into_result`, which is the last use of the args
        // before they are dropped
        unsafe { self.ctx.as_ref().unwrap_unchecked() }
    }

    /// Add an argument to the list.
    ///
    /// Arguments beyond the number the args were created with are moved to the heap.
    pub fn push_arg<T: IntoJs<'js>>(&mut self, arg: T) -> Result<()> {
        let v = arg.into_js(self.ctx())?.into_js_value();

        match &mut self.args {
            ArgsSlice::Stack { slice, offset } => {
                let index = *offset as usize;
                if index < ARGS_ON_STACK {
                    slice[index] = v;
                    *offset += 1;
                } else {
                    self.spill_arg(v);
                }
            }
            ArgsSlice::Heap(h) => h.push(v),
        }

        Ok(())
    }

    /// Moves the full stack arguments to the heap and adds `v`.
    #[cold]
    fn spill_arg(&mut self, v: qjs::JSValue) {
        let mut heap = Vec::with_capacity(ARGS_ON_STACK * 2);
        heap.extend_from_slice(self.values());
        heap.push(v);
        self.args = ArgsSlice::Heap(heap);
    }

    /// Add multiple arguments to the list.
    pub fn push_args<T, I>(&mut self, iter: I) -> Result<()>
    where
        T: IntoJs<'js>,
        I: IntoIterator<Item = T>,
    {
        for a in iter.into_iter() {
            self.push_arg(a)?
        }
        Ok(())
    }

    /// Add a this arguments.
    pub fn this<T>(&mut self, this: T) -> Result<()>
    where
        T: IntoJs<'js>,
    {
        let v = this.into_js(self.ctx())?;
        let v = mem::replace(&mut self.this, v.into_js_value());
        unsafe { qjs::JS_FreeValue(self.raw_ctx.as_ptr(), v) };
        Ok(())
    }

    /// Replace the this value with 'Undefined' and return the original value.
    pub fn take_this(&mut self) -> Value<'js> {
        let value = mem::replace(&mut self.this, qjs::JS_UNDEFINED);
        Value {
            ctx: self.ctx().clone(),
            value,
        }
    }

    /// The arguments currently in the list.
    #[inline]
    fn values(&self) -> &[qjs::JSValue] {
        match &self.args {
            ArgsSlice::Stack { slice, offset } => &slice[..*offset as usize],
            ArgsSlice::Heap(h) => h.as_slice(),
        }
    }

    /// The number of arguments currently in the list.
    #[inline]
    fn len(&self) -> usize {
        match &self.args {
            ArgsSlice::Stack { offset, .. } => *offset as usize,
            ArgsSlice::Heap(h) => h.len(),
        }
    }

    #[inline]
    fn as_ptr(&self) -> *const qjs::JSValue {
        match &self.args {
            ArgsSlice::Stack { slice, .. } => slice.as_ptr(),
            ArgsSlice::Heap(h) => h.as_ptr(),
        }
    }

    /// Call a function with the current set of arguments.
    pub fn apply<R>(mut self, func: &Function<'js>) -> Result<R>
    where
        R: FromJs<'js>,
    {
        let val = unsafe {
            #[cfg(feature = "parallel")]
            qjs::JS_UpdateStackTop(qjs::JS_GetRuntime(self.raw_ctx.as_ptr()));

            qjs::JS_Call(
                self.raw_ctx.as_ptr(),
                func.as_js_value(),
                self.this,
                self.len() as _,
                self.as_ptr() as _,
            )
        };
        unsafe { self.into_result(func.ctx(), val) }
    }

    /// Converts the owned result `val` of calling a function with these arguments; the arguments
    /// are freed after that, when the caller drops `self`, which it must do right away.
    ///
    /// `func_ctx` is the context of the called function. In the common case of it being the
    /// context of the arguments, it keeps the context alive until the arguments are freed, so the
    /// context reference of the arguments is moved to the result value instead of being cloned.
    #[inline]
    unsafe fn into_result<R>(&mut self, func_ctx: &Ctx<'js>, val: qjs::JSValue) -> Result<R>
    where
        R: FromJs<'js>,
    {
        let val = if qjs::JS_VALUE_GET_NORM_TAG(val) == qjs::JS_TAG_EXCEPTION {
            self.ctx().handle_exception(val)?
        } else {
            val
        };
        if self.raw_ctx == func_ctx.as_raw() {
            let ctx = self.ctx.take().unwrap_unchecked();
            R::from_js(func_ctx, Value::from_js_value(ctx, val))
        } else {
            let ctx = self.ctx();
            R::from_js(ctx, Value::from_js_value(ctx.clone(), val))
        }
    }

    pub fn defer(mut self, func: Function<'js>) -> Result<()> {
        let this = self.take_this();
        self.push_arg(this)?;
        self.push_arg(func)?;
        let ctx = self.ctx();
        unsafe {
            if qjs::JS_EnqueueJob(
                ctx.as_ptr(),
                Some(defer_call_job),
                self.len() as _,
                self.as_ptr() as _,
            ) < 0
            {
                return Err(ctx.raise_exception());
            }
        }
        Ok(())
    }

    pub fn construct<R>(mut self, constructor: &Constructor<'js>) -> Result<R>
    where
        R: FromJs<'js>,
    {
        let value = if unsafe { qjs::JS_VALUE_GET_TAG(self.this) != qjs::JS_TAG_UNDEFINED } {
            unsafe {
                #[cfg(feature = "parallel")]
                qjs::JS_UpdateStackTop(qjs::JS_GetRuntime(self.raw_ctx.as_ptr()));

                qjs::JS_CallConstructor2(
                    self.raw_ctx.as_ptr(),
                    constructor.as_js_value(),
                    self.this,
                    self.len() as _,
                    self.as_ptr() as _,
                )
            }
        } else {
            unsafe {
                #[cfg(feature = "parallel")]
                qjs::JS_UpdateStackTop(qjs::JS_GetRuntime(self.raw_ctx.as_ptr()));

                qjs::JS_CallConstructor(
                    self.raw_ctx.as_ptr(),
                    constructor.as_js_value(),
                    self.len() as _,
                    self.as_ptr() as _,
                )
            }
        };
        unsafe { self.into_result(constructor.ctx(), value) }
    }
}

impl Drop for Args<'_> {
    fn drop(&mut self) {
        // the values are freed before the `ctx` field is dropped
        let ctx = self.raw_ctx.as_ptr();
        for v in self.values() {
            unsafe { qjs::JS_FreeValue(ctx, *v) };
        }
        unsafe { qjs::JS_FreeValue(ctx, self.this) };
    }
}

/// A trait for converting values into arguments.
pub trait IntoArg<'js> {
    /// The number of arguments this value produces.
    fn num_args(&self) -> usize;

    /// Convert the value into an argument.
    fn into_arg(self, args: &mut Args<'js>) -> Result<()>;
}

/// A trait for converting a tuple of values into a list arguments.
pub trait IntoArgs<'js> {
    /// The number of arguments this value produces.
    fn num_args(&self) -> usize;

    /// Convert the value into an argument.
    fn into_args(self, args: &mut Args<'js>) -> Result<()>;

    fn apply<R>(self, function: &Function<'js>) -> Result<R>
    where
        R: FromJs<'js>,
        Self: Sized,
    {
        let mut args = Args::new(function.ctx().clone(), self.num_args());
        self.into_args(&mut args)?;
        args.apply(function)
    }

    fn defer<R>(self, function: Function<'js>) -> Result<()>
    where
        Self: Sized,
    {
        let mut args = Args::new(function.ctx().clone(), self.num_args());
        self.into_args(&mut args)?;
        args.defer(function)
    }

    fn construct<R>(self, function: &Constructor<'js>) -> Result<()>
    where
        Self: Sized,
    {
        let mut args = Args::new(function.ctx().clone(), self.num_args());
        self.into_args(&mut args)?;
        args.construct(function)
    }
}

impl<'js, T: IntoJs<'js>> IntoArg<'js> for T {
    fn num_args(&self) -> usize {
        1
    }

    fn into_arg(self, args: &mut Args<'js>) -> Result<()> {
        args.push_arg(self)
    }
}

impl<'js, T: IntoJs<'js>> IntoArg<'js> for This<T> {
    fn num_args(&self) -> usize {
        0
    }

    fn into_arg(self, args: &mut Args<'js>) -> Result<()> {
        args.this(self.0)
    }
}

impl<'js, T: IntoJs<'js>> IntoArg<'js> for Opt<T> {
    fn num_args(&self) -> usize {
        self.0.is_some() as usize
    }

    fn into_arg(self, args: &mut Args<'js>) -> Result<()> {
        if let Some(x) = self.0 {
            args.push_arg(x)?
        }
        Ok(())
    }
}

impl<'js, T: IntoJs<'js>> IntoArg<'js> for Rest<T> {
    fn num_args(&self) -> usize {
        self.0.len()
    }

    fn into_arg(self, args: &mut Args<'js>) -> Result<()> {
        args.push_args(self.0)
    }
}

impl<'js, T: IntoArgs<'js>> IntoArg<'js> for Flat<T> {
    fn num_args(&self) -> usize {
        self.0.num_args()
    }

    fn into_arg(self, args: &mut Args<'js>) -> Result<()> {
        self.0.into_args(args)
    }
}

macro_rules! impl_into_args {
    ($($t:ident),*) => {
        #[allow(non_snake_case)]
        impl<'js $(,$t)*> IntoArgs<'js> for ($($t,)*)
        where
            $($t : IntoArg<'js>,)*
        {
            fn num_args(&self) -> usize{
                let ($(ref $t,)*) = *self;
                0 $(+ $t.num_args())*
            }

            fn into_args(self, _args: &mut Args<'js>) -> Result<()>{
                let ($($t,)*) = self;
                $($t.into_arg(_args)?;)*
                Ok(())
            }
        }
    };
}

impl_into_args!();
impl_into_args!(A);
impl_into_args!(A, B);
impl_into_args!(A, B, C);
impl_into_args!(A, B, C, D);
impl_into_args!(A, B, C, D, E);
impl_into_args!(A, B, C, D, E, F);
impl_into_args!(A, B, C, D, E, F, G);
