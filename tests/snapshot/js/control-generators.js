// Generators, async generators and for-in enumeration across hibernations.

function* counter(start) {
    let i = start;
    try {
        while (true) {
            const cmd = yield i;
            if (cmd === "reset")
                i = start;
            else
                i++;
        }
    } finally {
        print("counter finally", start, i);
    }
}
var gStart = counter(10); // suspended at start
var gMid = counter(0); // suspended at a yield
gMid.next();
gMid.next();
var gDone = counter(5); // completed
gDone.next();
gDone.return("early");
var gThrow = counter(100);
gThrow.next();
var gReturnStart = counter(7); // return() before starting: no finally

function* inner() {
    const x = yield "a";
    yield "b:" + x;
    return "inner-ret";
}
function* delegating() {
    const r = yield* inner();
    print("delegate got", r);
    yield "after";
}
var gStar = delegating(); // suspended inside yield*
gStar.next();

function* fib() {
    let [a, b] = [0, 1];
    for (;;) {
        yield a;
        [a, b] = [b, a + b];
    }
}
var gFib = fib();
for (let k = 0; k < 6; k++)
    gFib.next();

// generator methods with 'this' and arguments
var genHolder = {
    base: 100,
    *items() {
        for (let i = 0; i < arguments.length; i++)
            yield this.base + arguments[i];
    },
};
var gMethod = genHolder.items(1, 2, 3);
gMethod.next();

// for-in inside generators, suspended mid enumeration
function* forInGen(o) {
    for (var k in o)
        yield k;
}
var forInObj = { x: 1, y: 2, z: 3, w: 4, 10: "ten" };
Object.defineProperty(forInObj, "hidden", { value: 0, enumerable: false });
var gForIn = forInGen(forInObj);
gForIn.next();
gForIn.next();
delete forInObj.z; // must be skipped after the restore
var protoObj = Object.create({ inherited1: "i1", own1: "shadowed", inherited2: "i2" });
protoObj.own1 = 1;
protoObj.own2 = 2;
var gForInProto = forInGen(protoObj);
gForInProto.next();
gForInProto.next();
gForInProto.next(); // in the prototype chain
var gForInArray = forInGen([10, 20, 30]);
gForInArray.next();
var gForInNull = forInGen(null);

function drain(g, n) {
    const out = [];
    for (let i = 0; i < n; i++) {
        const r = g.next();
        out.push(r.done ? "done:" + r.value : r.value);
        if (r.done)
            break;
    }
    return out.join(",");
}

function step1() {
    print("gStart", JSON.stringify(gStart.next()), JSON.stringify(gStart.next()), JSON.stringify(gStart.next("reset")));
    print("gMid", JSON.stringify(gMid.next()), JSON.stringify(gMid.return("ret")), JSON.stringify(gMid.next()));
    print("gDone", JSON.stringify(gDone.next()), JSON.stringify(gDone.return(1)));
    try {
        gThrow.throw(new Error("boom"));
    } catch (e) {
        print("gThrow", e.message, JSON.stringify(gThrow.next()));
    }
    print("gReturnStart", JSON.stringify(gReturnStart.return("r")), JSON.stringify(gReturnStart.next()));
    print("gStar", JSON.stringify(gStar.next("X")), JSON.stringify(gStar.next()), JSON.stringify(gStar.next()));
    print("gFib", drain(gFib, 5));
    print("gForIn", drain(gForIn, 10));
    print("gForInProto", drain(gForInProto, 10));
}

function step2() {
    print("gMethod", drain(gMethod, 5));
    print("gForInArray", drain(gForInArray, 5), drain(gForInNull, 2));
    try {
        gStart.throw("thrown");
    } catch (e) {
        print("gStart throw", e);
    }
}

// async generators
async function* ticker(name, n) {
    try {
        for (let i = 0; i < n; i++) {
            await null;
            yield name + i;
        }
        return name + "-end";
    } finally {
        print("ticker finally", name);
    }
}

// several queued requests
var agQueued = ticker("q", 3);
[agQueued.next(), agQueued.next(), agQueued.next(), agQueued.next(), agQueued.return("r"), agQueued.next()]
    .forEach((p, i) => p.then(r => print("queued", i, JSON.stringify(r))));

// for await over an async generator
(async () => {
    const seen = [];
    for await (const v of ticker("fa", 3))
        seen.push(v);
    print("for await done", seen.join(","));
})();

// return() and throw() while suspended at a yield
var agReturn = ticker("ret", 10);
agReturn.next()
    .then(r => { print("agReturn first", JSON.stringify(r)); return agReturn.return("stop"); })
    .then(r => print("agReturn return", JSON.stringify(r)))
    .then(() => agReturn.next())
    .then(r => print("agReturn after", JSON.stringify(r)));
var agThrow = ticker("thr", 10);
agThrow.next()
    .then(() => agThrow.throw(new Error("agboom")))
    .catch(e => print("agThrow", e.message));

// awaiting a pending promise inside an async generator
var gate;
var gatePromise = new Promise(r => { gate = r; });
async function* gated() {
    const v = await gatePromise;
    yield "gate:" + v;
    yield* [1, 2]; // async-from-sync iterator while suspended
    yield* ticker("inner", 2);
}
var agGated = gated();
var gatedResults = [agGated.next(), agGated.next(), agGated.next(), agGated.next(), agGated.next(), agGated.next()];
Promise.all(gatedResults).then(rs => print("gated", JSON.stringify(rs)));
Promise.resolve().then(() => null).then(() => gate("open"));

// for await over a sync iterable of promises (async-from-sync iterator)
var syncDeferred;
(async () => {
    const out = [];
    for await (const v of [Promise.resolve("p1"), "plain", new Promise(r => { syncDeferred = r; })])
        out.push(v);
    print("for await sync", out.join(","));
})();
Promise.resolve().then(() => null).then(() => null).then(() => syncDeferred("p3"));

// async generator suspended at start, used in onResume
var agStart = ticker("start", 2);
var agCompleted = ticker("completed", 0);
agCompleted.next().then(r => print("agCompleted", JSON.stringify(r)));

Promise.resolve().then(step1).then(step2);

globalThis.onResume = function () {
    agStart.next().then(r => print("agStart", JSON.stringify(r)));
    agStart.return("bye").then(r => print("agStart return", JSON.stringify(r)));
    agCompleted.next().then(r => print("agCompleted again", JSON.stringify(r)));
    agCompleted.return(Promise.resolve("awaited return")).then(r => print("agCompleted return", JSON.stringify(r)));
    agCompleted.throw("t").catch(e => print("agCompleted throw", e));
};
