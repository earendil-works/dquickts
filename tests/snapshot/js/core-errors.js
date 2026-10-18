// Stack traces: the [[ErrorData]] stack of the Error objects, CallSite
// objects, Error.captureStackTrace() and the Error constructor
// configuration must survive hibernation.
function thrower() { null.x; }
function Ctor() { this.e = new RangeError("in ctor"); }

var thrown;
try { thrower(); } catch (e) { thrown = e; }
var created = new TypeError("created");
var captured = {};
Error.captureStackTrace(captured);
var notCaptured = Reflect.construct(Error, ["no stack"], Object);
var overridden = new Error("overridden");
overridden.stack = "custom stack";

var callsites;
Error.prepareStackTrace = (err, frames) => { callsites = frames; return frames; };
var prepared = new Ctor().e;
[1].map(function mapper() { return new Error("native frame"); });
Error.prepareStackTrace = function (err, frames) { return "prepared:" + err.message + ":" + frames.length; };
Error.stackTraceLimit = 3;

var get = Object.getOwnPropertyDescriptor(Error.prototype, "stack").get;

function describe(cs) {
    return [cs.getFunctionName(), typeof cs.getFunction(), cs.getFileName() !== null,
            cs.getLineNumber(), cs.getColumnNumber(), cs.isNative(),
            cs.isConstructor()].join("/");
}

function report() {
    print("thrown", thrown.stack);
    print("created", created.stack, Object.prototype.hasOwnProperty.call(created, "stack"));
    print("captured", captured.stack);
    print("not captured", notCaptured.stack, get.call(notCaptured));
    print("overridden", overridden.stack, get.call(overridden));
    print("prepared", prepared.stack.length, prepared.stack.map(describe).join(" | "));
    print("callsites", callsites.map(describe).join(" | "));
    print("config", Error.stackTraceLimit, new Error("now").stack);
    print("thrown now", (function () { try { thrower(); } catch (e) { return e.stack; } })());
}
report();
// called by the host after the last hibernation
function onResume() {
    Error.prepareStackTrace = undefined;
    print("default now", new Error("again").stack);
    Error.stackTraceLimit = 10;
    report();
}
