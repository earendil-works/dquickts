use rquickjs::{
    class::Trace, CatchResultExt, Class, Context, Function, JsLifetime, Result, Runtime,
};

#[derive(Trace, JsLifetime)]
#[rquickjs::class]
pub struct Counter {
    value: u32,
}

#[rquickjs::methods]
impl Counter {
    #[qjs(constructor)]
    pub fn new(value: u32) -> Self {
        Counter { value }
    }

    pub fn get(&self) -> u32 {
        self.value
    }

    pub fn inc(&mut self) -> u32 {
        self.value += 1;
        self.value
    }

    /// Calls `f` while `self` is borrowed.
    pub fn with<'js>(&self, f: Function<'js>) -> Result<u32> {
        f.call::<_, u32>(())
    }

    /// Calls `f` while `self` is borrowed mutably.
    pub fn with_mut<'js>(&mut self, f: Function<'js>) -> Result<u32> {
        self.value += 1;
        f.call::<_, u32>(())
    }

    #[qjs(static)]
    pub fn sum(a: &Counter, b: &Counter) -> u32 {
        a.value + b.value
    }

    #[qjs(static)]
    pub fn add_to(target: &mut Counter, from: &Counter) -> u32 {
        target.value += from.value;
        target.value
    }
}

#[derive(Trace, JsLifetime)]
#[rquickjs::class(frozen)]
pub struct Frozen {
    value: u32,
}

#[rquickjs::methods]
impl Frozen {
    #[qjs(constructor)]
    pub fn new(value: u32) -> Self {
        Frozen { value }
    }

    pub fn get(&self) -> u32 {
        self.value
    }

    pub fn set(&mut self, value: u32) {
        self.value = value;
    }
}

pub fn main() {
    let rt = Runtime::new().unwrap();
    let ctx = Context::full(&rt).unwrap();

    ctx.with(|ctx| {
        Class::<Counter>::define(&ctx.globals()).unwrap();
        Class::<Frozen>::define(&ctx.globals()).unwrap();

        ctx.eval::<(), _>(
            r#"
            function assertEq(a, b) {
                if (a !== b) throw new Error(`expected ${b}, got ${a}`);
            }
            function assertThrows(f, kind, message) {
                try {
                    f();
                } catch (e) {
                    if (!(e instanceof kind)) throw new Error(`unexpected error ${e}`);
                    if (!String(e.message).includes(message)) {
                        throw new Error(`unexpected message ${e.message}`);
                    }
                    return;
                }
                throw new Error("expected an error");
            }

            const c = new Counter(1);
            const d = new Counter(10);
            assertEq(c.get(), 1);
            assertEq(c.inc(), 2);

            // shared borrows nest, a mutable borrow fails while borrowed and is released after
            assertEq(c.with(() => c.get() + c.with(() => c.get())), 4);
            assertThrows(() => c.with(() => c.inc()), Error, "already borrowed");
            assertThrows(() => c.with_mut(() => c.get()), Error, "already borrowed");
            assertEq(c.get(), 3);
            assertEq(c.with_mut(() => d.inc()), 11);
            assertEq(c.inc(), 5);

            // borrowed arguments, the same object twice
            assertEq(Counter.sum(c, d), 16);
            assertEq(Counter.sum(c, c), 10);
            assertEq(Counter.add_to(d, c), 16);
            assertThrows(() => Counter.add_to(c, c), Error, "already borrowed");
            assertEq(c.get(), 5);

            // not an instance of the class
            assertThrows(() => c.get.call({}), TypeError, "Counter");
            assertThrows(() => c.get.call(new Frozen(1)), TypeError, "Counter");
            assertThrows(() => c.inc.call(undefined), TypeError, "Counter");
            assertThrows(() => Counter.sum(c, {}), TypeError, "Counter");
            assertThrows(() => Counter.add_to(1, c), TypeError, "Counter");
            assertEq(c.get(), 5);

            // a frozen class can't be borrowed mutably
            const f = new Frozen(7);
            assertEq(f.get(), 7);
            assertThrows(() => f.set(8), Error, "not writable");
            assertEq(f.get(), 7);
        "#,
        )
        .catch(&ctx)
        .unwrap();
    });
}
