// Async functions, promises and their resolving functions across
// hibernations (one hibernation before every job).

function deferred() {
    let resolve, reject;
    const promise = new Promise((res, rej) => { resolve = res; reject = rej; });
    return { promise, resolve, reject };
}

// async functions awaiting across many jobs, with closures and try/finally
async function worker(name, steps) {
    const acc = [];
    const add = x => acc.push(name + x);
    try {
        for (let i = 0; i < steps; i++) {
            const v = await i;
            add(v);
            if (i === 2) {
                try {
                    await Promise.reject(new Error("inner " + name));
                } catch (e) {
                    add(":" + e.message);
                } finally {
                    add(":fin");
                }
            }
        }
        return acc.join(",");
    } finally {
        print("worker finally", name, acc.length);
    }
}
worker("A", 5).then(v => print("A done", v));
worker("B", 3).then(v => print("B done", v));

// 'this', arguments, thenables and nested awaits
var holder = {
    v: 7,
    async method(a, b) {
        const t = await { then(r) { r(a * 2); } };
        const inner = await (async () => { await null; return this.v + arguments.length; })();
        return [t, inner, b];
    },
};
holder.method(3, "x").then(r => print("method", JSON.stringify(r)));

// an async function that throws after several awaits
async function failing() {
    await 1;
    await 2;
    throw new TypeError("failed late");
}
failing().catch(e => print("failing", e.name, e.message));

// promise chain with then/catch/finally
Promise.resolve(1)
    .then(x => x + 1)
    .then(x => { throw new Error("chain " + x); })
    .catch(e => { print("caught", e.message); return "recovered"; })
    .finally(() => print("finally ran"))
    .then(v => print("chain end", v));
Promise.reject(new RangeError("fin-rej"))
    .finally(() => print("finally on rejection"))
    .then(null, e => print("finally kept", e.name));

// combinators with pending elements, settled over several jobs
var d1 = deferred(), d2 = deferred(), d3 = deferred(), d4 = deferred();
Promise.all([d1.promise, 2, d2.promise]).then(v => print("all", JSON.stringify(v)));
Promise.allSettled([d1.promise, d3.promise, Promise.reject("r0")])
    .then(v => print("allSettled", JSON.stringify(v)));
Promise.any([d3.promise, d4.promise]).then(v => print("any", v), e => print("any rejected", e.errors));
Promise.any([Promise.reject(1), d3.promise])
    .catch(e => print("any aggregate", e.constructor.name, JSON.stringify(e.errors)));
Promise.race([d4.promise, d2.promise]).then(v => print("race", v));
Promise.allKeyed({ k1: d1.promise, k2: "plain" }).then(v => print("allKeyed", JSON.stringify(v)));

// the resolving functions of d1..d4 are only called after hibernations
var steps = [
    () => d1.resolve("one"),
    () => d3.reject("three"),
    () => { d2.resolve("two"); d2.resolve("ignored"); d2.reject("ignored too"); },
    () => d4.resolve("four"),
];
steps.reduce((p, f) => p.then(f), Promise.resolve());

// a resolve/reject pair whose other half already ran
var pair = deferred();
pair.resolve("first wins");
Promise.resolve().then(() => pair.reject("too late")).then(() => pair.promise)
    .then(v => print("pair", v));

// neither function of the pair has run before the hibernations: once
// resolve() is called with a thenable, the promise is still pending but
// reject() must be ignored (the 'already resolved' flag is shared)
var untouched = deferred();
untouched.promise.then(v => print("untouched", v), e => print("untouched rejected", e));
Promise.resolve().then(() => null).then(() => {
    untouched.resolve({ then(r) { r("via thenable"); } });
    untouched.reject("must be ignored");
});

// unhandled rejection, handled after hibernations
var lateRejected = Promise.reject(new Error("late"));
Promise.resolve().then(() => null).then(() => {
    lateRejected.catch(e => print("late handled", e.message));
    lateRejected.catch(e => print("late handled twice", e.message));
});
// handled before being rejected: no tracker output
var handledEarly = deferred();
handledEarly.promise.then(null, e => print("handled early", e));
Promise.resolve().then(() => handledEarly.reject("early-reason"));

// Promise.withResolvers / Promise.try
var wr = Promise.withResolvers();
wr.promise.then(v => print("withResolvers", v));
Promise.try((a, b) => a + b, 20, 22).then(v => print("try", v));

// thenable resolution job pending in the queue at hibernation
var thenable = { then(res) { print("thenable then called"); res("from thenable"); } };
Promise.resolve().then(() => thenable).then(v => print("thenable", v));

// a promise subclass
class MyPromise extends Promise {
    tag() { return "my"; }
}
var mp = new MyPromise(r => r(5)).then(x => x * 2);
print("subclass", mp instanceof MyPromise, mp.tag());
mp.then(v => print("subclass value", v));

// async closures in a loop sharing state
var shared = { n: 0 };
var loopers = [];
for (let i = 0; i < 3; i++) {
    loopers.push((async () => {
        for (let j = 0; j < 3; j++) {
            await null;
            shared.n += i * 10 + j;
        }
        return i;
    })());
}
Promise.all(loopers).then(v => print("loopers", JSON.stringify(v), shared.n));

globalThis.onResume = function () {
    wr.resolve("resolved in onResume");
    print("onResume", shared.n);
    worker("C", 4).then(v => print("C done", v));
    Promise.reject("never handled");
};
