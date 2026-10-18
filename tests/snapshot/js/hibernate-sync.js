// Long synchronous computation: hibernated many times in the middle of
// loops, nested calls, closures and exception handlers.
function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

function work(seed) {
    let acc = seed;
    const items = [];
    const bump = () => acc++; // closure over a local of a suspended frame
    for (let i = 0; i < 200000; i++) {
        acc = (acc * 31 + i) % 1000003;
        if (i % 40000 === 0) {
            items.push(acc);
            bump();
        }
    }
    return { acc, items, f: fib(16) };
}

function outer(k) {
    const local = "outer" + k;
    const r = work(k);
    // operand stack temporaries live across the nested call above
    return local + ":" + r.acc + ":" + r.items.join(",") + ":" + r.f;
}

function sloppy(a, b) {
    // mapped arguments alias the parameters across suspensions
    for (let i = 0; i < 100000; i++)
        arguments[0] = a + 1;
    return [a, b, arguments.length].join("/");
}

function withFinally() {
    let log = [];
    try {
        for (let i = 0; i < 100000; i++)
            if (i === 99999) throw new Error("late " + i);
    } catch (e) {
        log.push("caught " + e.message);
    } finally {
        log.push("finally");
    }
    return log.join(",");
}

const bound = function (x, y) {
    let s = 0;
    for (let i = 0; i < 100000; i++) s += x * y;
    return this.tag + s;
}.bind({ tag: "bound:" }, 3);

class Accumulator {
    constructor(n) {
        this.total = 0;
        for (let i = 0; i < n; i++) this.total += i % 7;
    }
}

function tail(n, acc) {
    "use strict";
    if (n === 0) return acc;
    for (let i = 0; i < 1000; i++) acc = (acc + i) % 997;
    return tail(n - 1, acc);
}

const results = [];
for (let k = 0; k < 3; k++)
    results.push(outer(k));
results.push(sloppy(1, 2));
results.push(withFinally());
results.push(bound(4));
results.push(new Accumulator(150000).total);
results.push(tail(300, 1));
print(results.join("\n"));

// a callback called by a builtin is not suspendable: the request is
// deferred until forEach returns
let sum = 0;
[1, 2, 3].forEach(x => { for (let i = 0; i < 100000; i++) sum += x; });
print("forEach", sum);

// generator bodies run through a native continuation: deferred too
function* gen() { for (let i = 0; i < 3; i++) { let s = 0; for (let j = 0; j < 100000; j++) s += j; yield s + i; } }
print("gen", [...gen()].join(","));

let thrown;
try {
    (function deep(n) { if (n > 0) return deep(n - 1); for (let i = 0; i < 100000; i++); throw new RangeError("from deep"); })(50);
} catch (e) {
    thrown = e.name + ": " + e.message;
}
print(thrown);
results.length;
