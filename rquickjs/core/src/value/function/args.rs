use crate::{
    function::{Flat, Opt, Rest, This},
    qjs, Ctx, FromJs, Function, IntoJs, Result, Value,
};
use alloc::vec::Vec;
use core::{
    mem::{self, ManuallyDrop},
    ptr,
};

use super::{ffi::defer_call_job, Constructor};

const ARGS_ON_STACK: usize = 4;

enum ArgsSlice {
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
    /// Only released on drop if `owns_ctx`, see `Args::new_borrowed`.
    ctx: ManuallyDrop<Ctx<'js>>,
    owns_ctx: bool,
    this: qjs::JSValue,
    args: ArgsSlice,
}

impl<'js> Args<'js> {
    /// Returns a new args with space for a give number of arguments
    #[inline]
    pub fn new(ctx: Ctx<'js>, args: usize) -> Args<'js> {
        Self::with_ctx(ManuallyDrop::new(ctx), true, args)
    }

    /// Returns new args using `ctx` without taking a reference to the context.
    ///
    /// # Safety
    /// The args must be dropped before `ctx` (or the value it was borrowed from), and the context
    /// of the args must not leave them other than by a clone.
    #[inline]
    pub(crate) unsafe fn new_borrowed(ctx: &Ctx<'js>, args: usize) -> Args<'js> {
        Self::with_ctx(ManuallyDrop::new(ptr::read(ctx)), false, args)
    }

    #[inline]
    fn with_ctx(ctx: ManuallyDrop<Ctx<'js>>, owns_ctx: bool, args: usize) -> Args<'js> {
        Args {
            ctx,
            owns_ctx,
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
            ctx: ManuallyDrop::new(ctx),
            owns_ctx: true,
            this: qjs::JS_UNDEFINED,
            args: ArgsSlice::Heap(Vec::new()),
        }
    }

    /// Returns the context associated with these arguments.
    #[inline]
    pub fn ctx(&self) -> &Ctx<'js> {
        &self.ctx
    }

    /// Add an argument to the list.
    ///
    /// Arguments beyond the number the args were created with are moved to the heap.
    #[inline]
    pub fn push_arg<T: IntoJs<'js>>(&mut self, arg: T) -> Result<()> {
        let v = arg.into_js_raw(self.ctx())?;
        self.push_raw(v);
        Ok(())
    }

    /// Adds the owned value `v` to the list.
    #[inline]
    fn push_raw(&mut self, v: qjs::JSValue) {
        match &mut self.args {
            ArgsSlice::Stack { slice, offset } if (*offset as usize) < ARGS_ON_STACK => {
                slice[*offset as usize] = v;
                *offset += 1;
            }
            _ => self.push_heap(v),
        }
    }

    /// Adds `v` to the heap arguments, moving the full stack arguments to the heap first.
    #[inline(never)]
    fn push_heap(&mut self, v: qjs::JSValue) {
        if let ArgsSlice::Stack { slice, .. } = self.args {
            // only called with a full stack
            let mut heap = Vec::with_capacity(ARGS_ON_STACK * 2);
            heap.extend_from_slice(&slice);
            self.args = ArgsSlice::Heap(heap);
        }
        if let ArgsSlice::Heap(h) = &mut self.args {
            h.push(v);
        }
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
    #[inline]
    pub fn this<T>(&mut self, this: T) -> Result<()>
    where
        T: IntoJs<'js>,
    {
        let v = this.into_js_raw(self.ctx())?;
        let v = mem::replace(&mut self.this, v);
        unsafe { qjs::JS_FreeValue(self.ctx.as_ptr(), v) };
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
        self.values().len()
    }

    #[inline]
    fn as_ptr(&self) -> *const qjs::JSValue {
        self.values().as_ptr()
    }

    /// Call a function with the current set of arguments.
    #[inline]
    pub fn apply<R>(self, func: &Function<'js>) -> Result<R>
    where
        R: FromJs<'js>,
    {
        let val = unsafe {
            #[cfg(feature = "parallel")]
            qjs::JS_UpdateStackTop(qjs::JS_GetRuntime(self.ctx.as_ptr()));

            qjs::JS_Call(
                self.ctx.as_ptr(),
                func.as_js_value(),
                self.this,
                self.len() as _,
                self.as_ptr() as _,
            )
        };
        unsafe { self.into_result(val) }
    }

    /// Converts the owned result `val` of calling a function with these arguments; the arguments
    /// are freed after that, when `self` is dropped.
    #[inline]
    unsafe fn into_result<R>(&self, val: qjs::JSValue) -> Result<R>
    where
        R: FromJs<'js>,
    {
        let ctx = self.ctx();
        let val = ctx.handle_exception(val)?;
        R::from_js_raw_owned(ctx, val)
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

    #[inline]
    pub fn construct<R>(self, constructor: &Constructor<'js>) -> Result<R>
    where
        R: FromJs<'js>,
    {
        let ctx = self.ctx.as_ptr();
        let value = unsafe {
            #[cfg(feature = "parallel")]
            qjs::JS_UpdateStackTop(qjs::JS_GetRuntime(ctx));

            if qjs::JS_VALUE_GET_TAG(self.this) != qjs::JS_TAG_UNDEFINED {
                qjs::JS_CallConstructor2(
                    ctx,
                    constructor.as_js_value(),
                    self.this,
                    self.len() as _,
                    self.as_ptr() as _,
                )
            } else {
                qjs::JS_CallConstructor(
                    ctx,
                    constructor.as_js_value(),
                    self.len() as _,
                    self.as_ptr() as _,
                )
            }
        };
        unsafe { self.into_result(value) }
    }
}

impl Drop for Args<'_> {
    #[inline]
    fn drop(&mut self) {
        // the values are freed before the context is released
        let ctx = self.ctx.as_ptr();
        for v in self.values() {
            unsafe { qjs::JS_FreeValue(ctx, *v) };
        }
        unsafe { qjs::JS_FreeValue(ctx, self.this) };
        if self.owns_ctx {
            unsafe { ManuallyDrop::drop(&mut self.ctx) };
        }
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

    #[inline]
    fn apply<R>(self, function: &Function<'js>) -> Result<R>
    where
        R: FromJs<'js>,
        Self: Sized,
    {
        // SAFETY: the args are dropped before the borrow of `function` ends
        let mut args = unsafe { Args::new_borrowed(function.ctx(), self.num_args()) };
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

    #[inline]
    fn construct<R>(self, function: &Constructor<'js>) -> Result<()>
    where
        Self: Sized,
    {
        // SAFETY: the args are dropped before the borrow of `function` ends
        let mut args = unsafe { Args::new_borrowed(function.ctx(), self.num_args()) };
        self.into_args(&mut args)?;
        args.construct(function)
    }
}

impl<'js, T: IntoJs<'js>> IntoArg<'js> for T {
    #[inline]
    fn num_args(&self) -> usize {
        1
    }

    #[inline]
    fn into_arg(self, args: &mut Args<'js>) -> Result<()> {
        args.push_arg(self)
    }
}

impl<'js, T: IntoJs<'js>> IntoArg<'js> for This<T> {
    #[inline]
    fn num_args(&self) -> usize {
        0
    }

    #[inline]
    fn into_arg(self, args: &mut Args<'js>) -> Result<()> {
        args.this(self.0)
    }
}

impl<'js, T: IntoJs<'js>> IntoArg<'js> for Opt<T> {
    #[inline]
    fn num_args(&self) -> usize {
        self.0.is_some() as usize
    }

    #[inline]
    fn into_arg(self, args: &mut Args<'js>) -> Result<()> {
        if let Some(x) = self.0 {
            args.push_arg(x)?
        }
        Ok(())
    }
}

impl<'js, T: IntoJs<'js>> IntoArg<'js> for Rest<T> {
    #[inline]
    fn num_args(&self) -> usize {
        self.0.len()
    }

    #[inline]
    fn into_arg(self, args: &mut Args<'js>) -> Result<()> {
        args.push_args(self.0)
    }
}

impl<'js, T: IntoArgs<'js>> IntoArg<'js> for Flat<T> {
    #[inline]
    fn num_args(&self) -> usize {
        self.0.num_args()
    }

    #[inline]
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
            #[inline]
            fn num_args(&self) -> usize{
                let ($(ref $t,)*) = *self;
                0 $(+ $t.num_args())*
            }

            #[inline]
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
