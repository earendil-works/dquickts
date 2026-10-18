/* Explicit resource management: 'using' and 'await using' declarations,
   DisposableStack, AsyncDisposableStack and SuppressedError */
import * as std from "std";
import { assert, assertThrows, assertArrayEquals } from "./assert.js";

var log;

function res(name, fail) {
    return {
        [Symbol.dispose]() {
            log.push(name);
            if (fail)
                throw new Error(name);
        }
    };
}

function asyncRes(name, fail) {
    return {
        async [Symbol.asyncDispose]() {
            log.push(name);
            await null;
            log.push(name + " done");
            if (fail)
                throw new Error(name);
        }
    };
}

function catchError(f) {
    try {
        f();
    } catch (e) {
        return e;
    }
    throw Error("exception expected");
}

async function catchAsyncError(f) {
    try {
        await f();
    } catch (e) {
        return e;
    }
    throw Error("exception expected");
}

function assertSyntaxError(src) {
    assertThrows(SyntaxError, () => std.evalScript(src));
}

/* pending jobs are run by awaiting enough times */
async function flushJobs() {
    for (var i = 0; i < 10; i++)
        await null;
}

function test_scopes() {
    function f() {
        using a = res("a"), n = null, u = undefined;
        {
            using b = res("b");
            log.push("block");
        }
        using c = res("c");
        log.push("body");
        return log.push("return");
    }
    log = [];
    assert(f(), 4);
    assertArrayEquals(log, ["block", "b", "body", "return", "c", "a"]);

    /* the binding is a constant visible to closures */
    log = [];
    (function () {
        using x = res("x");
        var get = () => x;
        assert(get(), x);
        assertThrows(TypeError, () => { x = 1; });
    })();
    assertArrayEquals(log, ["x"]);

    /* break, continue and labelled break */
    log = [];
    for (var i = 0; i < 3; i++) {
        using r = res("r" + i);
        if (i == 0)
            continue;
        log.push("break");
        break;
    }
    assertArrayEquals(log, ["r0", "break", "r1"]);
    log = [];
    outer: {
        using a = res("a");
        for (;;) {
            using b = res("b");
            break outer;
        }
    }
    assertArrayEquals(log, ["b", "a"]);

    /* switch case: in a nested block */
    log = [];
    switch (1) {
    case 1: {
        using s = res("s");
        log.push("case");
    }
    }
    assertArrayEquals(log, ["case", "s"]);

    /* class static block */
    log = [];
    class C {
        static {
            using s = res("static");
            log.push("init");
        }
    }
    assertArrayEquals(log, ["init", "static"]);

    /* resources created by eval code in a block */
    log = [];
    eval("{ using e = res('eval'); log.push('eval body'); }");
    assertArrayEquals(log, ["eval body", "eval"]);
}

function test_loops() {
    /* for (;;) head: disposed when the loop ends */
    log = [];
    for (using h = res("h"), n = null; log.length < 2;)
        log.push("it");
    assertArrayEquals(log, ["it", "it", "h"]);

    /* for-of: disposed at the end of each iteration */
    log = [];
    for (using x of [res("x0"), null, res("x2")])
        log.push("it");
    assertArrayEquals(log, ["it", "x0", "it", "it", "x2"]);

    /* break: the resource is disposed before the iterator is closed */
    var iterable = {
        [Symbol.iterator]() {
            var i = 0;
            return {
                next() { return { value: res("v" + i++), done: false }; },
                return() { log.push("return"); return {}; },
            };
        }
    };
    log = [];
    for (using v of iterable) {
        log.push("it");
        if (log.length > 2)
            break;
    }
    assertArrayEquals(log, ["it", "v0", "it", "v1", "return"]);

    /* a non disposable value: the iterator is closed */
    log = [];
    var e = catchError(() => {
        for (using v of [res("ok"), 42])
            log.push("it");
    });
    assert(e instanceof TypeError, true);
    assertArrayEquals(log, ["it", "ok"]);

    /* each iteration has its own binding */
    log = [];
    var fns = [];
    for (using v of [res("a"), res("b")])
        fns.push(() => v);
    assert(fns[0]() !== fns[1](), true);
    assertArrayEquals(log, ["a", "b"]);
}

function test_generators() {
    function* g() {
        using a = res("a"), b = yield 1;
        yield 2;
    }
    var it;

    log = [];
    it = g();
    it.next();
    assert(it.return(5).value, 5);
    assertArrayEquals(log, ["a"]);

    log = [];
    it = g();
    it.next();
    it.next(res("b"));
    assert(catchError(() => it.throw(new Error("t"))).message, "t");
    assertArrayEquals(log, ["b", "a"]);

    log = [];
    assertArrayEquals([...g()], [1, 2]);
    assertArrayEquals(log, ["a"]);
}

function test_errors() {
    var e;

    /* the exception of the body is kept */
    log = [];
    e = catchError(() => {
        using a = res("a");
        throw new RangeError("body");
    });
    assert(e instanceof RangeError, true);
    assertArrayEquals(log, ["a"]);

    /* a disposal error is thrown as is */
    log = [];
    e = catchError(() => {
        using a = res("a"), b = res("b", true);
        return 1;
    });
    assert(e.message, "b");
    assertArrayEquals(log, ["b", "a"]);

    /* several errors: SuppressedError(new error, previous error) */
    log = [];
    e = catchError(() => {
        using a = res("a", true), b = res("b"), c = res("c", true);
        throw new Error("body");
    });
    assertArrayEquals(log, ["c", "b", "a"]);
    assert(e instanceof SuppressedError, true);
    assert(e.error.message, "a");
    assert(e.suppressed instanceof SuppressedError, true);
    assert(e.suppressed.error.message, "c");
    assert(e.suppressed.suppressed.message, "body");
    assert(Object.getOwnPropertyDescriptor(e, "error").enumerable, false);
    assert(e.hasOwnProperty("message"), false);

    /* the previous bindings are disposed if an initializer throws */
    log = [];
    e = catchError(() => {
        using a = res("a"), b = (() => { throw new Error("init"); })();
        log.push("not reached");
    });
    assert(e.message, "init");
    assertArrayEquals(log, ["a"]);

    /* a disposal error replaces a pending return */
    function f() {
        using a = res("a", true);
        return "ret";
    }
    log = [];
    assert(catchError(f).message, "a");

    /* the dispose method is checked and read once, at the declaration */
    var reads = 0, o = {};
    Object.defineProperty(o, Symbol.dispose, {
        get() { reads++; return () => log.push("got"); }
    });
    log = [];
    (function () { using x = o; log.push("body"); })();
    assert(reads, 1);
    assertArrayEquals(log, ["body", "got"]);
    for (var v of [42, "str", {}, { [Symbol.dispose]: 1 },
                   { [Symbol.asyncDispose]() {} }]) {
        log = [];
        e = catchError(() => { using x = v; log.push("body"); });
        assert(e instanceof TypeError, true);
        assertArrayEquals(log, []);
    }

    /* the SuppressedError is created from the intrinsic */
    var save = SuppressedError.prototype.constructor;
    SuppressedError.prototype.constructor = function () { throw Error("hijacked"); };
    e = catchError(() => {
        using a = res("a", true), b = res("b", true);
    });
    SuppressedError.prototype.constructor = save;
    assert(e instanceof SuppressedError, true);
    assert(e.error.message, "a");
}

function test_syntax() {
    /* 'using' is a contextual keyword */
    assert(std.evalScript("var using = [10]; using[0]"), 10);
    assert(std.evalScript("var using = 1, x = 2; using\nx"), 2);
    assert(std.evalScript("var r = []; for (using in { a: 1 }) r.push(using); for (using of [2]) r.push(using); r.join()"), "a,2");
    assert(std.evalScript("(function () { using await = null; using yield = null; return 1; })()"), 1);
    assert(std.evalScript("(function () { for (using of = null; ;) return 1; })()"), 1);
    assert(std.evalScript("(function () { let using = 3; return using; })()"), 3);
    /* 'await using' expression followed by 'x' */
    assert(typeof std.evalScript("(async function () { await using\nx; })"), "function");

    /* not allowed at the top level of scripts, in case clauses and in
       single statement contexts */
    assertSyntaxError("using x = null;");
    assertSyntaxError("switch (1) { case 1: using x = null; }");
    assertSyntaxError("(function () { switch (1) { default: using x = null; } })");
    assertSyntaxError("(function () { if (1) using x = null; })");
    assertSyntaxError("(function () { l: using x = null; })");
    assertSyntaxError("(function () { while (0) using x = null; })");
    assertSyntaxError("(function () { for (using x in {}); })");
    assertSyntaxError("(function () { for (using x = null of []); })");
    assertSyntaxError("(function () { using x; })");
    assertSyntaxError("(function () { using x = null, y; })");
    assertSyntaxError("(function () { using x = null, [y] = []; })");
    assertSyntaxError("(function () { using x = null, { y } = {}; })");
    assertSyntaxError("(function () { using let = null; })");
    assertSyntaxError("(function () { using x = null; let x; })");
    assertSyntaxError("(function* () { using yield = null; })");
    assertSyntaxError("(function () { await using x = null; })");
    assertSyntaxError("(async function () { await\nusing x = null; })");
    assertSyntaxError("(async function () { if (1) await using x = null; })");
    assertSyntaxError("(async function () { switch (1) { case 1: await using x = null; } })");
}

/* the disposals of 'await using' are awaited; 'log' and the order of
   the jobs prove the number of Awaits */
async function test_await_using() {
    var p, e;

    log = [];
    p = (async function () {
        await using a = asyncRes("a"), b = res("b"), n = null;
        using c = res("c");
        log.push("body");
    })();
    log.push("sync");
    await p;
    /* c is a sync disposal; b falls back to Symbol.dispose: its result
       is not awaited but its disposal is */
    assertArrayEquals(log, ["body", "c", "b", "sync", "a", "a done"]);

    /* null or undefined values: one Await at the end */
    async function nulls() {
        {
            await using x = null, y = undefined;
        }
        log.push("after");
    }
    log = [];
    p = nulls();
    log.push("sync");
    Promise.resolve().then(() => log.push("t1")).then(() => log.push("t2"));
    await p;
    await flushJobs();
    assertArrayEquals(log, ["sync", "after", "t1", "t2"]);

    /* not awaited if the declaration is not evaluated */
    async function notEvaluated() {
        {
            if (log.length >= 0)
                return log.push("return");
            await using x = null;
        }
    }
    log = [];
    p = notEvaluated();
    log.push("sync");
    Promise.resolve().then(() => log.push("t1"));
    await p;
    assertArrayEquals(log, ["return", "sync", "t1"]);

    /* a sync disposal after a null 'await using' awaits first */
    async function syncAfterNull() {
        using s = res("s");
        await using n = null;
        log.push("body");
    }
    log = [];
    p = syncAfterNull();
    log.push("sync");
    await p;
    assertArrayEquals(log, ["body", "sync", "s"]);

    /* the result of a Symbol.dispose fallback is not awaited */
    var thenCalls = 0;
    log = [];
    await (async function () {
        await using x = { [Symbol.dispose]() { return { then() { thenCalls++; } }; } };
    })();
    assert(thenCalls, 0);

    /* errors and rejections are merged */
    log = [];
    e = await catchAsyncError(async () => {
        await using a = asyncRes("a", true), b = res("b", true);
        throw new Error("body");
    });
    assert(e instanceof SuppressedError, true);
    assert(e.error.message, "a");
    assert(e.suppressed.error.message, "b");
    assert(e.suppressed.suppressed.message, "body");

    /* for-of, for await and async generators */
    log = [];
    for (await using x of [asyncRes("x0"), null])
        log.push("it");
    for await (using y of [res("y0")])
        log.push("it");
    assertArrayEquals(log, ["it", "x0", "x0 done", "it", "it", "y0"]);

    async function* ag() {
        await using a = asyncRes("ag");
        yield 1;
        yield 2;
    }
    log = [];
    var it = ag();
    await it.next();
    assert((await it.return(3)).value, 3);
    assertArrayEquals(log, ["ag", "ag done"]);

    /* module top level */
    globalThis.usingModuleLog = [];
    var m = await import("./fixture_using_module.js");
    assert(m.value, "module body");
    assertArrayEquals(globalThis.usingModuleLog,
                      ["module body", "dispose b", "dispose a"]);
    e = await catchAsyncError(() => import("./fixture_using_export.js"));
    assert(e instanceof SyntaxError, true);
}

function test_disposable_stack() {
    var s, s2, e;

    assert(DisposableStack.length, 0);
    assertThrows(TypeError, () => DisposableStack());
    s = new DisposableStack();
    assert(Object.prototype.toString.call(s), "[object DisposableStack]");
    assert(DisposableStack.prototype[Symbol.dispose], DisposableStack.prototype.dispose);

    log = [];
    var v = res("use");
    assert(s.use(v), v);
    assert(s.use(null), null);
    assert(s.adopt(7, (x) => log.push("adopt " + x)), 7);
    assert(s.defer(function () { log.push("defer " + this); }), undefined);
    assertThrows(TypeError, () => s.use(1));
    assertThrows(TypeError, () => s.use({}));
    assertThrows(TypeError, () => s.adopt(1, 2));
    assertThrows(TypeError, () => s.defer({}));
    assert(s.disposed, false);
    assert(s.dispose(), undefined);
    assertArrayEquals(log, ["defer undefined", "adopt 7", "use"]);
    assert(s.disposed, true);
    assert(s.dispose(), undefined);
    assertThrows(ReferenceError, () => s.use(null));
    assertThrows(ReferenceError, () => s.defer(() => {}));
    assertThrows(ReferenceError, () => s.move());
    assertThrows(TypeError, () => DisposableStack.prototype.dispose.call({}));

    /* move() */
    log = [];
    s = new DisposableStack();
    s.use(res("a"));
    s2 = s.move();
    assert(s.disposed, true);
    assert(Object.getPrototypeOf(s2), DisposableStack.prototype);
    class Sub extends DisposableStack {}
    assert(Object.getPrototypeOf(new Sub().move()), DisposableStack.prototype);
    s.dispose();
    assertArrayEquals(log, []);
    s2.dispose();
    assertArrayEquals(log, ["a"]);

    /* errors */
    log = [];
    s = new DisposableStack();
    s.use(res("a", true));
    s.use(res("b", true));
    e = catchError(() => s.dispose());
    assert(e instanceof SuppressedError, true);
    assert(e.error.message, "a");
    assert(e.suppressed.message, "b");

    /* the stack cannot be used by its disposers */
    log = [];
    s = new DisposableStack();
    s.defer(() => {
        assertThrows(ReferenceError, () => s.use(res("late")));
        assert(s.dispose(), undefined);
        log.push("reentrant");
    });
    s.dispose();
    assertArrayEquals(log, ["reentrant"]);

    /* a GC while disposing: the disposed resources are released */
    s = new DisposableStack();
    s.adopt({}, () => { std.gc(); });
    s.use({ [Symbol.dispose]() {} });
    s.dispose();

    /* with 'using' */
    log = [];
    (function () {
        using stack = new DisposableStack();
        stack.defer(() => log.push("deferred"));
        log.push("body");
    })();
    assertArrayEquals(log, ["body", "deferred"]);
}

async function test_async_disposable_stack() {
    var s, e, p;

    assertThrows(TypeError, () => AsyncDisposableStack());
    s = new AsyncDisposableStack();
    assert(AsyncDisposableStack.prototype[Symbol.asyncDispose],
           AsyncDisposableStack.prototype.disposeAsync);

    /* each disposal is awaited before the next one */
    log = [];
    s.use(asyncRes("a"));
    s.use(res("b"));
    s.defer(async () => { log.push("defer"); });
    s.adopt(3, async (x) => { log.push("adopt " + x); });
    p = s.disposeAsync();
    log.push("sync");
    assert(await p, undefined);
    assertArrayEquals(log, ["adopt 3", "sync", "defer", "b", "a", "a done"]);
    assert(s.disposed, true);
    assert(await s.disposeAsync(), undefined);
    assertThrows(ReferenceError, () => s.use(null));

    /* a null value is awaited */
    s = new AsyncDisposableStack();
    s.use(null);
    log = [];
    p = s.disposeAsync().then(() => log.push("disposed"));
    Promise.resolve().then(() => log.push("t1")).then(() => log.push("t2"));
    await p;
    await flushJobs();
    assertArrayEquals(log, ["t1", "disposed", "t2"]);

    /* errors and rejections */
    s = new AsyncDisposableStack();
    s.use(asyncRes("a", true));
    s.defer(() => { throw new Error("b"); });
    log = [];
    e = await catchAsyncError(() => s.disposeAsync());
    assert(e instanceof SuppressedError, true);
    assert(e.error.message, "a");
    assert(e.suppressed.message, "b");
    e = await catchAsyncError(() => AsyncDisposableStack.prototype.disposeAsync.call({}));
    assert(e instanceof TypeError, true);

    /* move() during a disposal, second call */
    s = new AsyncDisposableStack();
    s.use(asyncRes("a"));
    log = [];
    p = s.disposeAsync();
    assertThrows(ReferenceError, () => s.move());
    await s.disposeAsync();
    await p;
    assertArrayEquals(log, ["a", "a done"]);

    /* the awaits do not use the patchable Promise.prototype.then */
    var then = Promise.prototype.then, thenCalls = 0;
    Promise.prototype.then = function (f, r) {
        thenCalls++;
        return then.call(this, f, r);
    };
    s = new AsyncDisposableStack();
    s.use(undefined);
    s.defer(() => {});
    s.disposeAsync();
    Promise.prototype.then = then;
    assert(thenCalls, 0);
    std.gc();
    await flushJobs();

    /* a GC while a SuppressedError is built (by Error.prepareStackTrace)
       sees the pending error once */
    var prepare = Error.prepareStackTrace;
    Error.prepareStackTrace = () => { std.gc(); return ""; };
    s = new AsyncDisposableStack();
    s.defer(() => { throw { a: 1 }; });
    s.defer(() => { throw { b: 2 }; });
    s.defer(() => { throw { c: 3 }; });
    e = await catchAsyncError(() => s.disposeAsync());
    Error.prepareStackTrace = prepare;
    std.gc();
    assert(e.error.a, 1);
    assert(e.suppressed.error.b, 2);
    assert(e.suppressed.suppressed.c, 3);
}

async function test_iterators() {
    var called = 0, args;
    var it = Object.create(Iterator.prototype);
    assert(it[Symbol.dispose](), undefined);
    it.return = function () { called++; args = arguments.length; return 1; };
    assert(it[Symbol.dispose](), undefined);
    assert(called, 1);
    assert(args, 0);
    log = [];
    (function () {
        using g = (function* () { try { yield 1; } finally { log.push("closed"); } })();
        g.next();
    })();
    assertArrayEquals(log, ["closed"]);

    var AsyncIteratorPrototype =
        Object.getPrototypeOf(Object.getPrototypeOf((async function* () {}).prototype));
    var ait = Object.create(AsyncIteratorPrototype);
    assert(await ait[Symbol.asyncDispose](), undefined);
    ait.return = async function () { called++; return 5; };
    assert(await ait[Symbol.asyncDispose](), undefined);
    assert(called, 2);
    ait.return = function () { throw new Error("ret"); };
    assert((await catchAsyncError(() => ait[Symbol.asyncDispose]())).message, "ret");
}

function test_suppressed_error() {
    var e = new SuppressedError(1, 2, "msg");
    assert(SuppressedError.length, 3);
    assert(Object.getPrototypeOf(SuppressedError), Error);
    assert(SuppressedError.prototype.name, "SuppressedError");
    assert(e.error, 1);
    assert(e.suppressed, 2);
    assert(e.message, "msg");
    assert(e.hasOwnProperty("cause"), false);
    e = SuppressedError(1, 2, undefined, { cause: 3 });
    assert(e instanceof SuppressedError, true);
    assert(e.hasOwnProperty("message"), false);
    assert(e.hasOwnProperty("cause"), false);
    assert(Object.keys(e).length, 0);
}

test_scopes();
test_loops();
test_generators();
test_errors();
test_syntax();
await test_await_using();
test_disposable_stack();
await test_async_disposable_stack();
await test_iterators();
test_suppressed_error();
