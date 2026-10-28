import { assert, assertThrows } from "./assert.js";

/* V8 stack trace API: Error.captureStackTrace(), Error.stackTraceLimit,
   Error.prepareStackTrace() with CallSite objects, and the
   Error.prototype.stack accessor. */

function capture_frames(f)
{
    var frames;
    Error.prepareStackTrace = (_, callsites) => { frames = callsites; return callsites; };
    try {
        f();
    } finally {
        Error.prepareStackTrace = undefined;
    }
    return frames;
}

function test_prepare_stack()
{
    var e, f, i, stacks = [];

    for (i = 0; i < 2; i++) {
        Error.prepareStackTrace = i ? (_, frames) => frames : undefined;
        try {
            throw new Error(""); // line 26
        } catch(_e) {
            stacks.push(_e.stack);
        }
    }
    Error.prepareStackTrace = undefined;
    e = { stack: stacks[1] };

    assert(Array.isArray(e.stack));
    f = e.stack[0];
    assert(Object.prototype.toString.call(f), "[object CallSite]");
    assert(f.getFunctionName(), "test_prepare_stack");
    assert(f.getFunction(), test_prepare_stack);
    assert(f.getFileName().endsWith("test_stack_trace.js"));
    assert(f.getLineNumber(), 26);
    /* same position as in the default stack trace */
    assert(stacks[0].split("\n")[0].endsWith(":26:" + f.getColumnNumber() + ")"));
    assert(!f.isNative());
    assert(!f.isConstructor());
    assertThrows(TypeError, () => f.getLineNumber.call({}));
    /* the prototype is shared and the class has no constructor */
    assert(Object.getPrototypeOf(f), Object.getPrototypeOf(e.stack[1]));
    assert(typeof globalThis.CallSite, "undefined");
}

function test_prepare_arguments()
{
    var args, self, e;

    Error.prepareStackTrace = function (err, frames) {
        args = [err, frames];
        self = this;
        return { custom: frames.length };
    };
    e = new TypeError("msg");
    Error.prepareStackTrace = undefined;

    assert(args[0], e);
    assert(self, Error);
    /* any value is accepted as the stack */
    assert(e.stack.custom, args[1].length);
    assert(e.stack, e.stack);
}

function test_stack_trace_limit()
{
    var frames, e;
    function rec(n) { if (n > 0) return rec(n - 1); return new Error("deep"); }

    assert(Error.stackTraceLimit, 10);
    e = rec(30);
    assert(e.stack.split("\n").filter(l => l.length).length, 10);

    Error.stackTraceLimit = 1;
    try {
        frames = capture_frames(function one() { new Error(""); });
    } finally {
        Error.stackTraceLimit = 10;
    }
    assert(frames.length, 1);
    assert(frames[0].getFunctionName(), "one");

    Error.stackTraceLimit = Infinity;
    assert(Error.stackTraceLimit, Infinity);
    assert(rec(30).stack.split("\n").filter(l => l.length).length > 30);

    /* no stack trace levels for the non positive numbers and the non
       numbers (no JS code is run to convert them) */
    for (const v of [-Infinity, NaN, -0, 0, -1, "5", { valueOf() { throw "evil"; } }]) {
        Error.stackTraceLimit = v;
        assert(Object.is(Error.stackTraceLimit, v));
        e = new Error("fail");
        assert(e.message, "fail");
        assert(e.stack, "");
    }
    delete Error.stackTraceLimit;
    assert(new Error().stack, "");
    Error.stackTraceLimit = 10;
    assert(new Error().stack.includes("test_stack_trace.js"));
}

function test_capture_stack_trace()
{
    var o = {}, d;
    const fun1 = () => { fun2(); };
    const fun2 = () => { fun3(); };
    const fun3 = () => { log_stack(); };
    function log_stack() {
        Error.captureStackTrace(o, fun3);
    }

    assertThrows(TypeError, () => Error.captureStackTrace());
    assertThrows(TypeError, () => Error.captureStackTrace(1));

    Error.captureStackTrace(o);
    assert(typeof o.stack, "string");
    assert(o.stack.includes("test_capture_stack_trace"));
    assert(!o.stack.includes("captureStackTrace"));
    d = Object.getOwnPropertyDescriptor(o, "stack");
    assert(d.writable && d.configurable && !d.enumerable);

    fun1();
    assert(!o.stack.includes("fun3"));
    assert(!o.stack.includes("log_stack"));
    assert(o.stack.includes("fun2"));
    assert(o.stack.includes("fun1"));

    /* function not in the stack: no level */
    Error.captureStackTrace(o, function absent() {});
    assert(o.stack, "");

    /* an own property is installed on the Error objects too */
    var e = new Error("x");
    Error.captureStackTrace(e);
    assert(Object.prototype.hasOwnProperty.call(e, "stack"));

    /* with Error.prepareStackTrace */
    Error.prepareStackTrace = (err, frames) => frames;
    Error.captureStackTrace(o);
    Error.prepareStackTrace = undefined;
    assert(o.stack[0].getFunctionName(), "test_capture_stack_trace");

    assertThrows(TypeError, () => Error.captureStackTrace(Object.freeze({})));
}

function test_callsite_constructor()
{
    var frames;
    function Thing() { void new Error(""); }
    function plain() { void new Error(""); }

    frames = capture_frames(() => plain());
    assert(frames[0].getFunctionName(), "plain");
    assert(!frames[0].isConstructor());

    frames = capture_frames(() => new Thing());
    assert(frames[0].getFunctionName(), "Thing");
    assert(frames[0].isConstructor());
    assert(!frames[1].isConstructor());

    class Base { constructor() { void new Error(""); } }
    class Derived extends Base {}
    frames = capture_frames(() => new Derived());
    assert(frames[0].isConstructor());
    assert(frames[1].getFunctionName(), "Derived");
    assert(frames[1].isConstructor());
}

function test_callsite_native()
{
    var frames, native, scripted;

    frames = capture_frames(() => [1].map(function m() { return new Error(); }));
    native = frames.find((f) => f.isNative());
    assert(native.getFunctionName(), "map");
    assert(native.getFunction(), Array.prototype.map);
    assert(native.getLineNumber(), null);
    assert(native.getColumnNumber(), null);
    assert(native.getFileName(), null);
    scripted = frames.find((f) => !f.isNative());
    assert(scripted.getFunctionName(), "m");
    assert(typeof scripted.getLineNumber(), "number");
    assert(typeof scripted.getColumnNumber(), "number");

    /* anonymous function */
    frames = capture_frames(() => (0, function () { new Error(); })());
    assert(frames[0].getFunctionName(), null);
}

function test_callsite_parse_error()
{
    var e;

    Error.prepareStackTrace = (_, frames) => frames;
    try {
        eval("function fun() {\n    let a = 1 +;\n}\n");
    } catch (_e) {
        e = _e;
    }
    Error.prepareStackTrace = undefined;
    assert(e instanceof SyntaxError);
    assert(e.stack[0].getFileName(), "<input>");
    assert(e.stack[0].getLineNumber(), 2);
    assert(typeof e.stack[0].getColumnNumber(), "number");
    assert(e.stack[0].getFunction(), null);
    assert(e.stack[0].getFunctionName(), null);
    assert(e.stack[1].getFunctionName(), "test_callsite_parse_error");
}

function test_prepare_reentrancy()
{
    var calls, e, inner;

    /* errors created by Error.prepareStackTrace() get the default stack
       trace: no recursion */
    calls = 0;
    Error.prepareStackTrace = (err, frames) => {
        calls++;
        inner = new Error("inner");
        try { null.x; } catch (e2) { inner.thrown = e2; }
        return "custom";
    };
    e = new Error("outer");
    Error.prepareStackTrace = undefined;
    assert(calls, 1);
    assert(e.stack, "custom");
    assert(typeof inner.stack, "string");
    assert(inner.stack.includes("test_prepare_reentrancy"));
    assert(typeof inner.thrown.stack, "string");

    /* the stack is captured once when the exception goes through
       several frames, even if the result is undefined */
    calls = 0;
    Error.prepareStackTrace = () => { calls++; return undefined; };
    function a() { b(); }
    function b() { c(); }
    function c() { null.x; }
    try { a(); } catch (e2) { e = e2; }
    try { throw e; } catch (e2) {}
    Error.prepareStackTrace = undefined;
    assert(calls, 1);
    assert(e.stack, undefined);

    /* a throwing Error.prepareStackTrace() gives the default stack trace
       and does not replace the exception */
    Error.prepareStackTrace = () => { throw new RangeError("prepare"); };
    try { c(); } catch (e2) { e = e2; }
    Error.prepareStackTrace = undefined;
    assert(e instanceof TypeError);
    assert(typeof e.stack, "string");
    assert(e.stack.includes("at c"));

    /* not called for the stack overflow */
    calls = 0;
    Error.prepareStackTrace = function() { calls++; };
    function f() { f(); }
    try {
        assertThrows(InternalError, f);
    } finally {
        Error.prepareStackTrace = undefined;
    }
    assert(calls, 0);

    /* accessors and proxies of the Error constructor are not called */
    Object.defineProperty(Error, "prepareStackTrace", {
        get() { throw new Error("getter called"); },
        configurable: true,
    });
    e = new Error();
    delete Error.prepareStackTrace;
    assert(typeof e.stack, "string");
}

function test_stack_accessor()
{
    var desc, get, set, e, o;

    desc = Object.getOwnPropertyDescriptor(Error.prototype, "stack");
    get = desc.get;
    set = desc.set;
    assert(typeof get, "function");
    assert(typeof set, "function");
    assert(!desc.enumerable && desc.configurable);

    e = new TypeError("msg");
    assert(!Object.prototype.hasOwnProperty.call(e, "stack"));
    assert(Object.getOwnPropertyNames(e).indexOf("stack"), -1);
    assert(typeof e.stack, "string");
    assert(e.stack.includes("test_stack_accessor"));
    assert(Error.prototype.stack, undefined);
    assert(get.call({}), undefined);
    assert(get.call(Object.create(Error.prototype)), undefined);
    assertThrows(TypeError, () => get.call(1));

    /* the setter creates an own data property */
    assertThrows(TypeError, () => set.call(e, 1));
    assertThrows(TypeError, () => set.call(Error.prototype, "x"));
    set.call(e, "custom");
    desc = Object.getOwnPropertyDescriptor(e, "stack");
    assert(desc.value === "custom" && desc.writable && desc.enumerable && desc.configurable);
    /* the getter still reads the [[ErrorData]] stack */
    assert(get.call(e).includes("test_stack_accessor"));
    delete e.stack;
    assert(e.stack.includes("test_stack_accessor"));
    e.stack = "assigned";
    assert(e.stack, "assigned");
    assert(Object.prototype.hasOwnProperty.call(e, "stack"));

    o = {};
    set.call(o, "plain");
    assert(o.stack, "plain");
    assertThrows(TypeError, () => set.call(Object.preventExtensions({}), "x"));

    /* the thrown errors have a stack trace */
    try { null.x; } catch (e2) { e = e2; }
    assert(e.stack.includes("test_stack_accessor"));
    assert(!Object.prototype.hasOwnProperty.call(e, "stack"));

    /* JSON and Object.assign do not see it */
    assert(JSON.stringify(new Error("m")), "{}");
    assert(Object.assign({}, new Error("m")).stack, undefined);

    /* errors created without the constructor */
    e = Reflect.construct(Error, ["m"], Object);
    assert(e.stack, undefined);
    assert(typeof get.call(e), "string");
}

/* the stack trace is formatted when it is first read: it must be the
   same as if it was formatted when the error was created */
function test_lazy_stack()
{
    var e, e1, e2, e3, f, s;

    function make() { return new Error("lazy"); }
    function thrower() { null.x; }
    e = make();
    s = e.stack;
    assert(s.includes("    at make (") && s.includes("test_lazy_stack"));
    /* the function names are read when the error is created */
    f = function () { return new RangeError("renamed"); };
    Object.defineProperty(f, "name", { value: "before" });
    e1 = f();
    Object.defineProperty(f, "name", { value: "after" });
    /* the configuration is read when the error is created */
    e2 = (function limited() { return new Error(); })();
    try { thrower(); } catch (ex) { e3 = ex; }
    Error.stackTraceLimit = 1;
    Error.prepareStackTrace = () => "prepared";
    try {
        assert(e1.stack.startsWith("    at before ("));
        assert(e2.stack.split("\n").length > 2);
        assert(e2.stack.startsWith("    at limited ("));
        assert(e3.stack.startsWith("    at thrower ("));
        assert(new Error().stack, "prepared");
    } finally {
        Error.stackTraceLimit = 10;
        Error.prepareStackTrace = undefined;
    }
    /* the captured functions may be collected */
    e = (new Function("return function collected() { return new TypeError('gc'); }"))()();
    std.gc();
    assert(e.stack.startsWith("    at collected (<input>:"));
    /* same string each time */
    assert(e.stack === e.stack);
    assert(make().stack.replace(/:\d+:\d+\)/g, ""), s.replace(/:\d+:\d+\)/g, ""));
}

test_prepare_stack();
test_prepare_arguments();
test_stack_trace_limit();
test_capture_stack_trace();
test_callsite_constructor();
test_callsite_native();
test_callsite_parse_error();
test_prepare_reentrancy();
test_stack_accessor();
test_lazy_stack();

/* at the end: the line numbers of the tests above are checked */
import * as std from "std";
