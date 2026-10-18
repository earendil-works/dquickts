// Map and Set: special keys, insertion order, deletions, hash tables and
// iterators that are suspended across hibernations.
function fmt(v) {
    if (typeof v === "symbol") return v.toString();
    if (typeof v === "bigint") return v + "n";
    if (typeof v === "number" && Object.is(v, -0)) return "-0";
    if (v === m) return "<m>";
    if (v === s) return "<s>";
    if (typeof v === "function") return "fn:" + v.name;
    if (Array.isArray(v)) return "[" + v.map(fmt).join(",") + "]";
    if (v && typeof v === "object") return "{" + (v.name ?? "?") + "}";
    return typeof v + ":" + String(v);
}
function dump(label, coll) {
    var parts = [];
    coll.forEach((v, k) => parts.push(coll instanceof Map ? fmt(k) + "=>" + fmt(v) : fmt(k)));
    print(label, coll.size, parts.join(" "));
}
function step(label, it) {
    var r = it.next();
    print(label, r.done, r.done ? "-" : fmt(r.value));
}

var ko = { name: "ko" };
var ka = [1, 2];
function kf() {}
var ksym = Symbol("ks");
var m = new Map([[ko, "object"], ["str", 1], [NaN, "nan"], [-0, "zero"], [ksym, "sym"],
                 [ka, ka], [1n, "big"], [12345678901234567890n, "bigger"], [true, "bool"],
                 [null, "null"], [undefined, "undef"], [2.5, "float"], ["", "empty"]]);
m.set(kf, m);
m.delete("str");
m.set("str", "reinserted");
m.set("rope" + "-" + String(m.size), "rope key");
var s = new Set(["a", ko, NaN, 0, -0, ksym, "b", "c", "d", 1n]);
s.delete("b");
var m3 = new Map([[NaN, "nan"], [-0, "negzero"], [0 / 0, "nan2"], ["0", "string zero"]]);

// many records: hash table resizes, lookups by value and by address
var big = new Map();
var bigKeys = [];
for (var i = 0; i < 200; i++) {
    var k = i % 3 === 0 ? { name: "o" + i } : i % 3 === 1 ? "k" + i : i * 1.5;
    bigKeys.push(k);
    big.set(k, i);
}
for (var i = 0; i < 200; i += 7)
    big.delete(bigKeys[i]);

// iterators in every state
var itEntries = m.entries(); itEntries.next(); itEntries.next();
var itKeys = m.keys(); itKeys.next();             // on ko, deleted after hibernation
var itValues = m.values();                         // not started
var itDone = s.entries(); while (!itDone.next().done);
var itZombie = m[Symbol.iterator]();
itZombie.next(); itZombie.next(); itZombie.next(); // on -0 (stored as 0)
m.delete(0);                                       // deleted record kept for itZombie
m.delete(NaN);                                     // previous record deleted too
var itSet = s.values(); itSet.next(); itSet.next();
var itSetEntries = s.entries(); itSetEntries.next();

// iterator on a cleared map: continues with records added later
var m2 = new Map([["x", 1], ["y", 2], ["z", 3]]);
var itCleared = m2.keys(); itCleared.next(); itCleared.next();
m2.clear();
var itFirstDeleted = new Set([1, 2, 3]);
var itFD = itFirstDeleted.values(); itFD.next();
itFirstDeleted.delete(1);

class MyMap extends Map {
    get tag() { return "MyMap:" + this.size; }
}
var sub = new MyMap([[1, "one"]]);
var grouped = Map.groupBy([1, 2, 3, 4, 5], x => x % 2 ? "odd" : "even");

function report(label) {
    print("--", label);
    dump("m", m);
    dump("s", s);
    print("lookups", m.get(ko), m.get(NaN), m.get(0), m.get(-0), m.get(ksym), fmt(m.get(ka)),
          m.get(1n), m.get(12345678901234567890n), m.get(true), m.get(null), m.get(undefined),
          m.get(2.5), m.get(""), m.get("str"), m.get(kf) === m, m.has("nope"), m.get("rope-14"));
    print("set has", s.has("a"), s.has(ko), s.has(NaN), s.has(0), s.has(-0), s.has(ksym), s.has("b"), s.has(1n));
    var sum = 0, found = 0;
    for (var i = 0; i < 200; i++) {
        if (big.has(bigKeys[i])) { found++; sum += big.get(bigKeys[i]); }
    }
    print("big", big.size, found, sum, [...big.keys()].slice(0, 6).map(fmt).join(","));
    dump("m2", m2);
    dump("m3", m3);
    print("m3 get", m3.get(NaN), m3.get(-0), m3.get(0), m3.get("0"), Object.is([...m3.keys()][1], 0));
    print("sub", sub.tag, sub instanceof MyMap, sub.get(1));
    print("grouped", [...grouped].map(([k, v]) => k + ":" + v.join("")).join(" "));
}
report("initial");

Promise.resolve().then(() => {
    report("job 1");
    step("itEntries", itEntries);
    step("itKeys", itKeys);
    m.delete(ko);                 // current record of itKeys
    step("itValues", itValues);
    step("itDone", itDone);
    step("itZombie", itZombie);
    step("itSet", itSet);
    m2.set("after-clear", 4);
}).then(() => {
    report("job 2");
    step("itKeys", itKeys);       // continues after the deleted record
    step("itZombie", itZombie);
    m.set("appended", "late");
    s.add("e");
    s.delete("c");
    step("itCleared", itCleared);
    step("itFD", itFD);
    step("itSetEntries", itSetEntries);
}).then(() => {
    report("job 3");
    var rest = [];
    for (var r of itEntries) rest.push(fmt(r));
    print("itEntries rest", rest.join(" "));
    rest = [];
    for (var r of itZombie) rest.push(fmt(r));
    print("itZombie rest", rest.join(" "));
    step("itSet", itSet);
    step("itSet", itSet);
    step("itCleared", itCleared);
    step("itFD", itFD);
    step("itFD", itFD);
});

function onResume() {
    report("resume");
    for (var i = 0; i < 200; i += 2)
        big.delete(bigKeys[i]);
    for (var i = 0; i < 50; i++)
        big.set("new" + i, i);
    var sum = 0;
    big.forEach(v => { sum += v; });
    print("big after", big.size, sum);
    m.clear();
    s.clear();
    print("cleared", m.size, s.size, m.get(ko), s.has("a"));
    step("itValues", itValues);
    step("itSetEntries", itSetEntries);
    m.set(ko, "again");
    step("itValues", itValues);
}
