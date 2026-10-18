// Iterator helpers and builtin iterators partially consumed before the
// hibernations, Array.fromAsync pending across them.

function* gen(n, tag) {
    try {
        for (let i = 0; i < n; i++)
            yield i;
    } finally {
        if (tag)
            print("gen closed", tag);
    }
}

function drain(it, n) {
    const out = [];
    for (let i = 0; i < n; i++) {
        const r = it.next();
        if (r.done) {
            out.push("done");
            break;
        }
        out.push(JSON.stringify(r.value));
    }
    return out.join(",");
}

var mapCalls = 0;
var itMap = [1, 2, 3, 4, 5].values().map((x, i) => { mapCalls++; return x * 10 + i; });
itMap.next();
itMap.next();
var itFilter = gen(20).filter((x, i) => x % 3 === 0 && i < 15);
itFilter.next();
var itTake = gen(100, "take").take(4);
itTake.next();
itTake.next();
var itDrop = gen(10).drop(3);
itDrop.next();
var itFlat = ["ab", "cde", "f"].values().flatMap(s => s.split(""));
itFlat.next();
itFlat.next();
itFlat.next(); // inside the second inner iterator
var itChunks = gen(8).chunks(3);
itChunks.next();
var itWindows = gen(6).windows(4, "allow-partial");
itWindows.next();
var itTakeReturn = gen(10, "takeReturn").take(5);
itTakeReturn.next();

var itConcat = Iterator.concat([1, 2], new String("xy"), gen(3), { [Symbol.iterator]() { return gen(2); } });
itConcat.next();
itConcat.next();
itConcat.next(); // inside the second iterable
var itConcatDone = Iterator.concat([1]);
itConcatDone.next();
itConcatDone.next();

var itZip = Iterator.zip([[1, 2, 3], new String("abcd"), gen(5, "zip")]);
itZip.next();
var itZipLong = Iterator.zip([[1], [1, 2, 3]], { mode: "longest", padding: ["P", "Q"] });
itZipLong.next();
itZipLong.next(); // the first iterator is exhausted
var itZipStrict = Iterator.zip([[1, 2], gen(3)], { mode: "strict" });
itZipStrict.next();
var itZipKeyed = Iterator.zipKeyed({ a: [1, 2, 3], b: gen(3), [Symbol("s")]: new String("xyz") });
itZipKeyed.next();
var itZipEmpty = Iterator.zip([]);

// iterators that keep yielding after they reported completion: the
// helpers must not call them again once they are done
function rogue(name, n) {
    let i = 0;
    return {
        [Symbol.iterator]() { return this; },
        next() { i++; return i === n ? { done: true, value: undefined } : { done: false, value: name + i }; },
        return() { print("rogue return", name); return {}; },
    };
}
var itClosed = Iterator.prototype.map.call(rogue("h", 100), x => "m" + x);
itClosed.next();
itClosed.return();
var itExhausted = Iterator.prototype.filter.call(rogue("f", 2), x => true);
itExhausted.next();
itExhausted.next();
var itZipRogue = Iterator.zip([rogue("z", 2), [1, 2, 3]], { mode: "longest", padding: ["pad"] });
itZipRogue.next();
itZipRogue.next(); // the rogue iterator is exhausted: padding from now on
var reentrant = {
    i: 0,
    next() { return { done: false, value: this.i++ }; },
    return() {
        try {
            itZipReentrant.next();
            print("reentrant next ok");
        } catch (e) {
            print("reentrant next", e.message);
        }
        return {};
    },
};
var itZipReentrant = Iterator.zip([reentrant]);
itZipReentrant.next(); // started: return() runs the closing in the executing state

var wrapped = { i: 0, next() { return { done: this.i >= 4, value: "w" + this.i++ }; }, return() { print("wrapped return"); return {}; } };
var itWrap = Iterator.from(wrapped);
itWrap.next();
var itWrapReturn = Iterator.from({ i: 0, next() { return { done: false, value: this.i++ }; }, return() { print("wrap2 return"); return {}; } });
itWrapReturn.next();

// builtin iterators
var arrEntries = ["a", "b", "c", "d"].entries();
arrEntries.next();
var arrKeys = [5, 6, 7].keys();
arrKeys.next();
var arrDone = [1].values();
arrDone.next();
arrDone.next();
var growing = [1, 2];
var arrGrowing = growing.values();
arrGrowing.next();
var argsIter = (function () { return arguments[Symbol.iterator](); })("x", "y", "z");
argsIter.next();
var strIt = "h\u20acllo\u{1F600}!"[Symbol.iterator]();
strIt.next();
strIt.next();
strIt.next();
strIt.next(); // before the surrogate pair
var reIt = "a1b22c333d4444".matchAll(/(\d)(\d*)/g);
reIt.next();
var reUnicode = "x\u{1F600}y\u{1F600}z".matchAll(/\u{1F600}|y/gu);
reUnicode.next();
var reEmpty = "abc".matchAll(/(?:)/g);
reEmpty.next();
var reDone = "q".matchAll(/q/g);
reDone.next();
reDone.next();
var reCustom = /o/g;
reCustom.lastIndex = 5;
var reFromSymbol = RegExp.prototype[Symbol.matchAll].call(reCustom, "foo boo zoo");

// Array.fromAsync
var gateResolve;
var gate = new Promise(r => { gateResolve = r; });
Array.fromAsync([1, gate, 3]).then(a => print("fromAsync array", JSON.stringify(a)));
async function* agen() {
    yield 1;
    await gate;
    yield 2;
    yield 3;
}
Array.fromAsync(agen(), function (x, i) { return x * this.mul + i; }, { mul: 3 })
    .then(a => print("fromAsync mapped", JSON.stringify(a)));
Array.fromAsync({ length: 3, 0: "x", 1: gate, 2: Promise.resolve("z") })
    .then(a => print("fromAsync arraylike", JSON.stringify(a)));
Array.fromAsync([1, 2], async x => { await null; return x + 0.5; })
    .then(a => print("fromAsync async mapper", JSON.stringify(a)));
Array.fromAsync(gen(3, "fromAsync"), x => { if (x === 1) throw new Error("mapper threw"); return x; })
    .catch(e => print("fromAsync error", e.message));
class MyArray extends Array {}
MyArray.fromAsync([gate]).then(a => print("fromAsync ctor", a instanceof MyArray, a.length, a[0]));
Promise.resolve().then(() => null).then(() => gateResolve("G"));

function report(tag) {
    print(tag, "map", drain(itMap, 2), mapCalls);
    print(tag, "filter", drain(itFilter, 2));
    print(tag, "take", drain(itTake, 1));
    print(tag, "drop", drain(itDrop, 2));
    print(tag, "flat", drain(itFlat, 2));
    print(tag, "chunks", drain(itChunks, 1));
    print(tag, "windows", drain(itWindows, 1));
    print(tag, "concat", drain(itConcat, 3));
    print(tag, "zip", drain(itZip, 1));
    print(tag, "zipLong", drain(itZipLong, 1));
    print(tag, "zipKeyed", JSON.stringify(itZipKeyed.next().value));
    print(tag, "wrap", drain(itWrap, 1));
    print(tag, "entries", drain(arrEntries, 1), "keys", drain(arrKeys, 1));
    print(tag, "str", drain(strIt, 1));
    const m = reIt.next().value;
    print(tag, "matchAll", m && [m[0], m[1], m[2], m.index].join("/"));
    print(tag, "matchAll unicode", JSON.stringify(reUnicode.next().value));
    print(tag, "matchAll empty", reEmpty.next().value.index);
}

Promise.resolve().then(() => report("job"));

globalThis.onResume = function () {
    report("resume");
    print("rest map", drain(itMap, 5), "filter", drain(itFilter, 5), "take", drain(itTake, 5));
    print("rest drop", drain(itDrop, 10), "flat", drain(itFlat, 5));
    print("rest chunks", drain(itChunks, 5), "windows", drain(itWindows, 5));
    print("rest concat", drain(itConcat, 10), drain(itConcatDone, 2));
    print("rest zip", drain(itZip, 5), "zipLong", drain(itZipLong, 5));
    try {
        drain(itZipStrict, 5);
    } catch (e) {
        print("zip strict", e.message);
    }
    print("rest zipKeyed", drain(itZipKeyed, 5), "zipEmpty", drain(itZipEmpty, 2));
    print("rest wrap", drain(itWrap, 5), JSON.stringify(itWrapReturn.return()));
    print("take return", JSON.stringify(itTakeReturn.return()), drain(itTakeReturn, 2));
    print("rest entries", drain(arrEntries, 5), "keys", drain(arrKeys, 5), "done", drain(arrDone, 2));
    growing.push(3);
    print("growing", drain(arrGrowing, 5), "args", drain(argsIter, 5));
    print("rest str", drain(strIt, 5));
    print("rest matchAll", [...reIt].map(m => m[0]).join(","), drain(reDone, 1));
    print("rest unicode", [...reUnicode].map(m => m.index).join(","), "empty", [...reEmpty].map(m => m.index).join(","));
    print("matchAll symbol", [...reFromSymbol].map(m => m.index).join(","), reCustom.lastIndex);
    print("closed helper", drain(itClosed, 2), "exhausted helper", drain(itExhausted, 2));
    print("zip rogue", drain(itZipRogue, 3));
    print("zip reentrant", JSON.stringify(itZipReentrant.return()), drain(itZipReentrant, 1));
};
