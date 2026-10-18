use super::{Persist, SnapshotHost, SnapshotKind};
use crate::{
    class::{JsClass, Trace, Tracer, Writable},
    function::{Opt, This},
    Class, Constructor, Context, Ctx, Error, Function, JsLifetime, Object, Result, Runtime, Value,
};

/// A Rust class with state saved by `Persist`.
struct Counter {
    count: i32,
}

impl<'js> Trace<'js> for Counter {
    fn trace<'a>(&self, _tracer: Tracer<'a, 'js>) {}
}

unsafe impl<'js> JsLifetime<'js> for Counter {
    type Changed<'to> = Counter;
}

impl<'js> JsClass<'js> for Counter {
    const NAME: &'static str = "Counter";

    type Mutable = Writable;

    fn prototype(ctx: &Ctx<'js>) -> Result<Option<Object<'js>>> {
        let proto = Object::new(ctx.clone())?;
        let inc = Function::new(ctx.clone(), |this: This<Class<'js, Counter>>| {
            let mut counter = this.0.borrow_mut();
            counter.count += 1;
            counter.count
        })?;
        proto.set("inc", inc.with_name("inc")?)?;
        Ok(Some(proto))
    }

    fn constructor(ctx: &Ctx<'js>) -> Result<Option<Constructor<'js>>> {
        let constructor = Constructor::new_class::<Counter, _, _>(
            ctx.clone(),
            |ctx: Ctx<'js>, start: Opt<i32>| {
                Class::instance(
                    ctx,
                    Counter {
                        count: start.0.unwrap_or(0),
                    },
                )
            },
        )?;
        Ok(Some(constructor))
    }
}

impl<'js> Persist<'js> for Counter {
    fn save(&self, ctx: &Ctx<'js>) -> Result<Value<'js>> {
        Ok(Value::new_int(ctx.clone(), self.count))
    }

    fn load(_ctx: &Ctx<'js>, saved: Value<'js>) -> Result<Self> {
        Ok(Counter {
            count: saved.get()?,
        })
    }
}

/// A class without prototype.
struct Token {
    id: i32,
}

impl<'js> Trace<'js> for Token {
    fn trace<'a>(&self, _tracer: Tracer<'a, 'js>) {}
}

unsafe impl<'js> JsLifetime<'js> for Token {
    type Changed<'to> = Token;
}

impl<'js> JsClass<'js> for Token {
    const NAME: &'static str = "Token";

    type Mutable = Writable;

    fn prototype(_ctx: &Ctx<'js>) -> Result<Option<Object<'js>>> {
        Ok(None)
    }

    fn constructor(_ctx: &Ctx<'js>) -> Result<Option<Constructor<'js>>> {
        Ok(None)
    }
}

impl<'js> Persist<'js> for Token {
    fn save(&self, ctx: &Ctx<'js>) -> Result<Value<'js>> {
        Ok(Value::new_int(ctx.clone(), self.id))
    }

    fn load(_ctx: &Ctx<'js>, saved: Value<'js>) -> Result<Self> {
        Ok(Token { id: saved.get()? })
    }
}

/// A class holding an instance of another class, which its loader creates.
struct Boxed<'js> {
    inner: Class<'js, Counter>,
}

impl<'js> Trace<'js> for Boxed<'js> {
    fn trace<'a>(&self, tracer: Tracer<'a, 'js>) {
        self.inner.trace(tracer)
    }
}

unsafe impl<'js> JsLifetime<'js> for Boxed<'js> {
    type Changed<'to> = Boxed<'to>;
}

impl<'js> JsClass<'js> for Boxed<'js> {
    const NAME: &'static str = "Boxed";

    type Mutable = Writable;

    fn prototype(ctx: &Ctx<'js>) -> Result<Option<Object<'js>>> {
        let proto = Object::new(ctx.clone())?;
        let inner = Function::new(ctx.clone(), |this: This<Class<'js, Boxed<'js>>>| {
            this.0.borrow().inner.clone()
        })?;
        proto.set("inner", inner)?;
        Ok(Some(proto))
    }

    fn constructor(ctx: &Ctx<'js>) -> Result<Option<Constructor<'js>>> {
        let constructor = Constructor::new_class::<Boxed, _, _>(ctx.clone(), |ctx: Ctx<'js>| {
            let inner = Class::instance(ctx.clone(), Counter { count: 0 })?;
            Class::instance(ctx, Boxed { inner })
        })?;
        Ok(Some(constructor))
    }
}

impl<'js> Persist<'js> for Boxed<'js> {
    fn save(&self, ctx: &Ctx<'js>) -> Result<Value<'js>> {
        Ok(Value::new_int(ctx.clone(), self.inner.borrow().count))
    }

    fn load(ctx: &Ctx<'js>, saved: Value<'js>) -> Result<Self> {
        let inner = Class::instance(
            ctx.clone(),
            Counter {
                count: saved.get()?,
            },
        )?;
        Ok(Boxed { inner })
    }
}

fn host() -> SnapshotHost {
    SnapshotHost::new()
        .function("add", |ctx| {
            Function::new(ctx.clone(), |a: i32, b: i32| a + b)?.with_name("add")
        })
        .class::<Counter>()
        .class::<Token>()
        .class::<Boxed<'static>>()
        .builtins()
}

/// A runtime whose global object has the natives of `host`.
fn setup(host: &SnapshotHost) -> (Runtime, Context) {
    let rt = Runtime::new().unwrap();
    let ctx = Context::full(&rt).unwrap();
    ctx.with(|ctx| -> Result<()> {
        let natives = host.natives(&ctx)?;
        let globals = ctx.globals();
        globals.set("add", natives.function("add")?)?;
        globals.set("Counter", natives.constructor::<Counter>()?)?;
        globals.set("snapshot", natives.function("snapshot")?)?;
        globals.set("restore", natives.function("restore")?)?;
        globals.set("Boxed", natives.constructor::<Boxed>()?)?;
        Ok(())
    })
    .unwrap();
    (rt, ctx)
}

fn read(host: &SnapshotHost, base: &[u8], delta: Option<&[u8]>) -> (Runtime, Context) {
    let rt = Runtime::new().unwrap();
    let restored = rt.read_snapshot(host, base, delta).unwrap();
    assert_eq!(restored.contexts.len(), 1);
    let ctx = restored.contexts[0].clone();
    (rt, ctx)
}

fn eval<V: for<'js> crate::FromJs<'js>>(ctx: &Context, src: &str) -> V {
    ctx.with(|ctx| ctx.eval::<V, _>(src)).unwrap()
}

fn write(ctx: &Context, host: &SnapshotHost, kind: SnapshotKind) -> Vec<u8> {
    ctx.with(|ctx| ctx.write_snapshot(host, kind, &[])).unwrap()
}

const LOOP: &str = "let s = 0; for (let i = 0; i < 300000; i++) s += i % 7; s";
const LOOP_RESULT: i32 = {
    let mut s = 0;
    let mut i = 0;
    while i < 300000 {
        s += i % 7;
        i += 1;
    }
    s
};

#[test]
fn suspend_and_resume() {
    let rt = Runtime::new().unwrap();
    let ctx = Context::full(&rt).unwrap();
    let suspender = rt.suspender();
    rt.set_interrupt_handler(Some(Box::new(move || {
        suspender.request();
        false
    })));
    let suspensions = ctx.with(|ctx| {
        let mut result = ctx.eval::<i32, _>(LOOP);
        let mut suspensions = 0;
        while let Err(Error::Suspended) = result {
            suspensions += 1;
            // the marker of the suspension is not an exception value
            assert!(ctx.catch().is_undefined());
            // other code can run while suspended
            assert_eq!(ctx.eval::<i32, _>("1 + 1").unwrap(), 2);
            result = ctx.resume();
        }
        assert_eq!(result.unwrap(), LOOP_RESULT);
        suspensions
    });
    assert!(suspensions > 10, "{suspensions} suspensions");
    assert!(!rt.is_suspended());
}

#[test]
fn natives_and_classes_across_runtimes() {
    let host = host();
    let (_rt, ctx) = setup(&host);
    eval::<()>(
        &ctx,
        "var c = new Counter(5); c.inc(); var total = add(1, 2);
         class Sub extends Counter { twice() { this.inc(); return this.inc(); } }
         var sub = new Sub(10);",
    );
    let state = write(&ctx, &host, SnapshotKind::Full);

    let (_rt2, ctx2) = read(&host, &state, None);
    let result: String = eval(
        &ctx2,
        "JSON.stringify([c.inc(), add(total, 10), c instanceof Counter,
          Object.getPrototypeOf(c) === Counter.prototype, new Counter().inc(),
          new Counter(1) instanceof Counter, sub.twice(), sub instanceof Sub])",
    );
    assert_eq!(result, "[7,13,true,true,1,true,12,true]");
    // Rust code creates instances with the restored prototype
    ctx2.with(|ctx| {
        let counter = Class::instance(ctx.clone(), Counter { count: 41 }).unwrap();
        ctx.globals().set("fromRust", counter).unwrap();
    });
    assert_eq!(
        eval::<i32>(&ctx2, "fromRust instanceof Counter ? fromRust.inc() : -1"),
        42
    );
}

#[test]
fn objects_not_identified_by_their_prototype() {
    let host = host();
    let (_rt, ctx) = setup(&host);
    ctx.with(|ctx| {
        let token = Class::instance(ctx.clone(), Token { id: 7 }).unwrap();
        ctx.globals().set("token", token).unwrap();
    });
    eval::<()>(
        &ctx,
        "var c = new Counter(3); Object.setPrototypeOf(c, null);
         var plainAdd = add; Object.setPrototypeOf(plainAdd, Counter.prototype);",
    );
    let state = write(&ctx, &host, SnapshotKind::Full);

    let (_rt2, ctx2) = read(&host, &state, None);
    assert_eq!(eval::<i32>(&ctx2, "Counter.prototype.inc.call(c)"), 4);
    assert_eq!(
        eval::<i32>(&ctx2, "Function.prototype.call.call(plainAdd, null, 1, 2)"),
        3
    );
    ctx2.with(|ctx| {
        let token: Class<Token> = ctx.globals().get("token").unwrap();
        assert_eq!(token.borrow().id, 7);
    });
}

#[test]
fn loaders_create_instances_with_the_restored_prototypes() {
    let host = host();
    let (_rt, ctx) = setup(&host);
    eval::<()>(&ctx, "var b = new Boxed(); b.inner().inc();");
    let state = write(&ctx, &host, SnapshotKind::Full);

    let (_rt2, ctx2) = read(&host, &state, None);
    let result: String = eval(
        &ctx2,
        "JSON.stringify([b.inner() instanceof Counter, b.inner().inc(), new Boxed().inner().inc()])",
    );
    assert_eq!(result, "[true,2,1]");
}

#[test]
fn base_and_deltas() {
    let host = host();
    let (_rt, ctx) = setup(&host);
    let mut script = String::from("var c = new Counter(); var items = [];");
    for i in 0..100 {
        script += &format!("function f{i}(x) {{ return x + {i}; }}");
    }
    eval::<()>(&ctx, &script);
    let base = write(&ctx, &host, SnapshotKind::Base);

    eval::<()>(&ctx, "c.inc(); items.push(f42(0));");
    let delta = write(&ctx, &host, SnapshotKind::Delta);
    assert!(
        delta.len() * 10 < base.len(),
        "base {} delta {}",
        base.len(),
        delta.len()
    );

    // the restored runtime writes deltas of the same base
    let (_rt2, ctx2) = read(&host, &base, Some(&delta));
    assert_eq!(
        eval::<String>(&ctx2, "JSON.stringify([c.inc(), items])"),
        "[2,[42]]"
    );
    let delta2 = write(&ctx2, &host, SnapshotKind::Delta);
    let (_rt3, ctx3) = read(&host, &base, Some(&delta2));
    assert_eq!(eval::<i32>(&ctx3, "c.inc() + f99(0)"), 102);

    // a delta needs its base
    let rt4 = Runtime::new().unwrap();
    assert!(matches!(
        rt4.read_snapshot(&host, &delta, None),
        Err(Error::Snapshot(_))
    ));
}

#[test]
fn suspended_execution_across_runtimes() {
    let host = host();
    let (rt, ctx) = setup(&host);
    let suspender = rt.suspender();
    rt.set_interrupt_handler(Some(Box::new(move || {
        suspender.request();
        false
    })));
    let state = ctx.with(|ctx| {
        assert!(matches!(ctx.eval::<i32, _>(LOOP), Err(Error::Suspended)));
        ctx.write_snapshot(&host, SnapshotKind::Full, &[]).unwrap()
    });

    let (rt2, ctx2) = read(&host, &state, None);
    assert!(rt2.is_suspended());
    assert_eq!(ctx2.with(|ctx| ctx.resume::<i32>()).unwrap(), LOOP_RESULT);
}

#[test]
fn roots() {
    let host = host();
    let (_rt, ctx) = setup(&host);
    let state = ctx.with(|ctx| {
        let value: Value = ctx.eval("({ answer: 42 })").unwrap();
        ctx.write_snapshot(&host, SnapshotKind::Full, &[value])
            .unwrap()
    });
    let rt2 = Runtime::new().unwrap();
    let restored = rt2.read_snapshot(&host, &state, None).unwrap();
    let root = restored.roots.into_iter().next().unwrap();
    let answer: i32 = restored.contexts[0]
        .with(|ctx| root.restore(&ctx)?.into_object().unwrap().get("answer"))
        .unwrap();
    assert_eq!(answer, 42);
}

#[test]
fn unsaveable_heap_and_mismatched_host() {
    let host = host();
    let (_rt, ctx) = setup(&host);
    ctx.with(|ctx| {
        let closure = Function::new(ctx.clone(), || 1).unwrap();
        ctx.globals().set("closure", closure).unwrap();
        let error = ctx.write_snapshot(&host, SnapshotKind::Full, &[]);
        assert!(matches!(error, Err(Error::Exception)));
        let message: String = ctx.catch().get::<Object>().unwrap().get("message").unwrap();
        assert!(message.contains("not one of the natives"), "{message}");
        ctx.globals().remove("closure").unwrap();
    });
    let state = write(&ctx, &host, SnapshotKind::Full);

    let other = SnapshotHost::new().function("add", |ctx| Function::new(ctx.clone(), || 0));
    let rt2 = Runtime::new().unwrap();
    assert!(matches!(
        rt2.read_snapshot(&other, &state, None),
        Err(Error::Snapshot(_))
    ));
}

/// Runs `src` and carries out the `restore()` calls in new runtimes.
fn run_with_restores(host: &SnapshotHost, ctx: Context, src: &str) -> (Context, String) {
    let mut ctx = ctx;
    let mut result = ctx.with(|ctx| ctx.eval::<String, _>(src));
    loop {
        match result {
            Err(Error::Restore(restore)) => {
                let rt = Runtime::new().unwrap();
                let restored = restore.apply(host, &rt).unwrap();
                ctx = restored.contexts[0].clone();
                result = ctx.with(|ctx| ctx.resume::<String>());
            }
            result => return (ctx, result.unwrap()),
        }
    }
}

#[test]
fn builtins_jump_back() {
    let host = host();
    let (_rt, ctx) = setup(&host);
    let (ctx, result) = run_with_restores(
        &host,
        ctx,
        "var runs = 0;
         function main() {
             const r = snapshot();
             runs++;
             if (r instanceof Uint8Array) {
                 try { restore(r, { again: true }); } finally { runs += 100; }
             }
             return JSON.stringify([r, runs]);
         }
         main()",
    );
    // `finally` did not run: the restored state did not see the first increment
    assert_eq!(result, r#"[{"again":true},1]"#);
    assert_eq!(eval::<i32>(&ctx, "runs"), 1);
}

#[cfg(feature = "std")]
#[test]
fn builtins_with_a_file() {
    let host = host();
    let (_rt, ctx) = setup(&host);
    let path = std::env::temp_dir().join(format!("rquickjs-snapshot-{}.bin", std::process::id()));
    let src = format!(
        "var path = {:?};
         var steps = [];
         function main() {{
             const r = snapshot(path);
             steps.push(String(r));
             if (r === undefined) restore(path);
             return steps.join();
         }}
         main()",
        path.to_str().unwrap()
    );
    let (_ctx, result) = run_with_restores(&host, ctx, &src);
    assert_eq!(result, "true");
    std::fs::remove_file(path).unwrap();
}

#[test]
fn builtins_where_not_suspendable() {
    let host = host();
    let (_rt, ctx) = setup(&host);
    let result: String = eval(
        &ctx,
        "const errors = [];
         for (const f of [() => [1].map(() => snapshot()), () => [1].map(() => restore(new Uint8Array(1))),
                          () => snapshot.call(null)]) {
             try { f(); errors.push('no error'); } catch (e) { errors.push(e.constructor.name); }
         }
         errors.join()",
    );
    assert_eq!(result, "TypeError,TypeError,TypeError");
    // the state cannot be saved: snapshot() throws
    ctx.with(|ctx| {
        let closure = Function::new(ctx.clone(), || 1).unwrap();
        ctx.globals().set("closure", closure).unwrap();
        let caught: bool = ctx
            .eval("try { snapshot(); false } catch (e) { e instanceof TypeError }")
            .unwrap();
        assert!(caught);
    });
}

#[test]
fn repeated_restores_keep_one_context() {
    let host = host();
    let (_rt, ctx) = setup(&host);
    eval::<()>(&ctx, "var n = 0; var c = new Counter();");
    let mut state = write(&ctx, &host, SnapshotKind::Full);
    let mut runtimes = Vec::new();
    for i in 1..=5 {
        let (rt, ctx) = read(&host, &state, None);
        assert_eq!(eval::<i32>(&ctx, "++n + c.inc() * 0"), i);
        state = write(&ctx, &host, SnapshotKind::Full);
        runtimes.push((rt, ctx));
    }
    let (_rt, ctx) = read(&host, &state, None);
    assert_eq!(eval::<String>(&ctx, "`${n} ${c.inc()}`"), "5 6");
}
