// 'using' declarations, DisposableStack and AsyncDisposableStack across
// hibernations: resources waiting for their disposal, disposals in
// progress (dispose methods, 'await using', disposeAsync()) and the
// functions created for them.

var log = [];
function res(name) {
    return { [Symbol.dispose]() { log.push("dispose " + name); } };
}
function spin(n) {
    var s = 0;
    for (var i = 0; i < n; i++)
        s = (s + i * 7) % 10007;
    return s;
}

// suspensions in the bodies and in the dispose methods
function sync(k) {
    using a = res("a" + k), n = null, b = {
        [Symbol.dispose]() { log.push("b" + k + " " + spin(20000)); }
    };
    {
        using c = res("c" + k);
        log.push("body " + spin(30000));
    }
    for (using d of [res("d" + k), null])
        log.push("iter " + spin(10000));
    try {
        using e = { [Symbol.dispose]() { spin(10000); throw new Error("e" + k); } };
        throw new Error("body" + k);
    } catch (err) {
        log.push(err.name + " " + err.error.message + " " + err.suppressed.message);
    }
    return k;
}
for (var k = 0; k < 3; k++)
    sync(k);
print(log.join("\n"));
log = [];

// resources of each kind waiting in a DisposableStack
var stack = new DisposableStack();
stack.use(res("use"));
stack.adopt("v", v => log.push("adopt " + v));
stack.defer(() => log.push("defer"));
var moved;

// an AsyncDisposableStack disposed across jobs
var astack = new AsyncDisposableStack();
astack.use({ async [Symbol.asyncDispose]() { await null; log.push("async a"); } });
astack.use(null);
astack.use(res("sync fallback"));
astack.defer(async () => { await null; throw new Error("deferred"); });

async function main() {
    await null;
    await using x = {
        async [Symbol.asyncDispose]() { await null; await null; log.push("await using x"); }
    }, y = res("fallback y"), z = null;
    using s = new DisposableStack();
    s.defer(() => log.push("inner stack"));
    moved = stack.move();
    await null;
    try {
        await astack.disposeAsync();
    } catch (e) {
        log.push("disposeAsync: " + e.message);
    }
    var proto = Object.getPrototypeOf(Object.getPrototypeOf((async function* () {}).prototype));
    var it = Object.create(proto);
    it.return = async () => { await null; log.push("iterator return"); };
    await it[Symbol.asyncDispose]();
    log.push("main body " + stack.disposed);
}

main().then(() => {
    moved.dispose();
    print(log.join("\n"));
    var e = new SuppressedError(new Error("x"), "y", "msg");
    print(e.name, e.message, e.error.message, e.suppressed);
});
