// DOMException objects (name, message, cached legacy code), btoa/atob
// and queueMicrotask jobs pending across hibernation.
var plain = new DOMException();
var named = new DOMException("not here", "NotFoundError");
var cached = new DOMException("cached", "AbortError");
cached.code; // computes and caches the legacy code
class Sub extends DOMException {
    constructor() { super("sub", "TimeoutError"); this.extra = 1; }
}
var sub = new Sub();
var thrown;
try { atob("*"); } catch (e) { thrown = e; }
var log = [];
queueMicrotask(() => log.push("microtask"));
Promise.resolve().then(() => log.push("then"));

function describe(e) {
    return [e.name, e.message, e.code, e instanceof DOMException,
            Error.isError(e), String(e), typeof e.stack].join("/");
}

function report() {
    print("plain", describe(plain));
    print("named", describe(named));
    print("cached", describe(cached));
    print("sub", describe(sub), sub instanceof Sub, sub.extra);
    print("thrown", describe(thrown));
    print("constants", DOMException.ABORT_ERR, DOMException.prototype.DATA_CLONE_ERR);
    print("base64", btoa("snapshot"), atob("c25hcHNob3Q="));
    print("log", log.join());
}
report();
function onResume() {
    report();
}
