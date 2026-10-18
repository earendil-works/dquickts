//! Tools for using different allocators with QuickJS.

use crate::qjs;

mod rust;

use alloc::boxed::Box;
use core::ptr;
pub use rust::RustAllocator;

/// The allocator interface
///
/// # Safety
/// Failure to implement this trait correctly will result in undefined behavior.
/// - `alloc` must return a either a null pointer or a pointer to an available region of memory
/// atleast `size` bytes and aligned to the size of `usize`.
/// - `realloc` must either return a null pointer or return a pointer to an available region of
/// memory atleast `new_size` bytes and aligned to the size of `usize`.
/// - `usable_size` must return the amount of available memory for any allocation allocated with
/// this allocator.
pub unsafe trait Allocator {
    /// Allocate new memory
    ///
    ///
    fn alloc(&mut self, size: usize) -> *mut u8;

    /// De-allocate previously allocated memory
    ///
    /// # Safety
    /// Caller must ensure that the pointer that is being deallocated was allocated by the same
    /// Allocator instance.
    unsafe fn dealloc(&mut self, ptr: *mut u8);

    /// Re-allocate previously allocated memory
    ///
    /// # Safety
    /// Caller must ensure that the pointer points to an allocation that was allocated by the same
    /// Allocator instance.
    unsafe fn realloc(&mut self, ptr: *mut u8, new_size: usize) -> *mut u8;

    /// Get usable size of allocated memory region
    ///
    /// # Safety
    /// Caller must ensure that the pointer handed to this function points to an allocation
    /// allocated by the same allocator instance.
    unsafe fn usable_size(ptr: *mut u8) -> usize
    where
        Self: Sized;
}

type DynAllocator = Box<dyn Allocator>;

/// Bookkeeping overhead of an allocation counted by QuickJS in its memory
/// usage (same as its default allocator).
const MALLOC_OVERHEAD: usize = 8;

#[derive(Debug)]
pub(crate) struct AllocatorHolder(*mut DynAllocator);

impl Drop for AllocatorHolder {
    fn drop(&mut self) {
        let _ = unsafe { Box::from_raw(self.0) };
    }
}

#[allow(clippy::extra_unused_type_parameters)]
impl AllocatorHolder {
    pub(crate) fn functions<A>() -> qjs::JSMallocFunctions
    where
        A: Allocator,
    {
        qjs::JSMallocFunctions {
            js_malloc: Some(Self::malloc::<A>),
            js_free: Some(Self::free::<A>),
            js_realloc: Some(Self::realloc::<A>),
            js_malloc_usable_size: Some(Self::malloc_usable_size::<A>),
        }
    }

    pub(crate) fn new<A>(allocator: A) -> Self
    where
        A: Allocator + 'static,
    {
        Self(Box::into_raw(Box::new(Box::new(allocator))))
    }

    pub(crate) fn opaque_ptr(&self) -> *mut DynAllocator {
        self.0
    }

    /// The QuickJS allocation hooks enforce the memory limit and keep the
    /// counters of `state` up to date, like its default allocator.
    unsafe extern "C" fn malloc<A>(
        state: *mut qjs::JSMallocState,
        size: qjs::size_t,
    ) -> *mut qjs::c_void
    where
        A: Allocator,
    {
        let state = &mut *state;
        let size: usize = size.try_into().expect(qjs::SIZE_T_ERROR);
        if (state.malloc_size as usize).saturating_add(size) > state.malloc_limit as usize {
            return ptr::null_mut();
        }
        let allocator = &mut *(state.opaque as *mut DynAllocator);
        let ptr = allocator.alloc(size);
        if ptr.is_null() {
            return ptr::null_mut();
        }
        state.malloc_count += 1;
        state.malloc_size += (A::usable_size(ptr) + MALLOC_OVERHEAD) as qjs::size_t;
        ptr as *mut qjs::c_void
    }

    unsafe extern "C" fn free<A>(state: *mut qjs::JSMallocState, ptr: *mut qjs::c_void)
    where
        A: Allocator,
    {
        // simulate the default behavior of libc::free
        if ptr.is_null() {
            return;
        }
        let state = &mut *state;
        state.malloc_count -= 1;
        state.malloc_size -= (A::usable_size(ptr as _) + MALLOC_OVERHEAD) as qjs::size_t;
        let allocator = &mut *(state.opaque as *mut DynAllocator);
        allocator.dealloc(ptr as _);
    }

    unsafe extern "C" fn realloc<A>(
        state: *mut qjs::JSMallocState,
        ptr: *mut qjs::c_void,
        size: qjs::size_t,
    ) -> *mut qjs::c_void
    where
        A: Allocator,
    {
        if ptr.is_null() {
            if size == 0 {
                return ptr::null_mut();
            }
            return Self::malloc::<A>(state, size);
        }
        if size == 0 {
            Self::free::<A>(state, ptr);
            return ptr::null_mut();
        }
        let state = &mut *state;
        let size: usize = size.try_into().expect(qjs::SIZE_T_ERROR);
        let old_size = A::usable_size(ptr as _);
        if (state.malloc_size as usize)
            .saturating_add(size)
            .saturating_sub(old_size)
            > state.malloc_limit as usize
        {
            return ptr::null_mut();
        }
        let allocator = &mut *(state.opaque as *mut DynAllocator);
        let ptr = allocator.realloc(ptr as _, size);
        if ptr.is_null() {
            return ptr::null_mut();
        }
        state.malloc_size = (state.malloc_size as usize + A::usable_size(ptr) - old_size) as _;
        ptr as *mut qjs::c_void
    }

    unsafe extern "C" fn malloc_usable_size<A>(ptr: *const qjs::c_void) -> qjs::size_t
    where
        A: Allocator,
    {
        // simulate the default behavior of libc::malloc_usable_size
        if ptr.is_null() {
            return 0;
        }
        A::usable_size(ptr as _).try_into().unwrap()
    }
}
