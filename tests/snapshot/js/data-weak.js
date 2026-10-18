// WeakMap, WeakSet, WeakRef and FinalizationRegistry. gc() is not
// available: entries whose key or target is only weakly held are created
// to exercise the dead paths, but only live ones are observed.
var k1 = { name: "k1" };
var k2 = { name: "k2" };
var k3 = [3];
var ksym = Symbol("weak key");
var wm = new WeakMap([[k1, "v1"], [k2, { back: k2 }], [ksym, "symbol value"]]);
wm.set({ name: "temp" }, "dead key");            // key dead right away
wm.set(k3, k1);
wm.delete(k2);
wm.set(k2, "k2 again");
var ws = new WeakSet([k1, k3, ksym]);
ws.add({ name: "temp" });
ws.add(Symbol("temp symbol"));

var target = { name: "target" };
var wr = new WeakRef(target);
var wrDead = new WeakRef({ name: "gone" });          // target dead right away
var wrSym = new WeakRef(ksym);
var wrFn = new WeakRef(report);

var calls = 0;
var fr = new FinalizationRegistry(held => { calls++; });
var tok = { name: "token" };
var held = { name: "held" };
fr.register(k1, held, tok);
fr.register(k3, "held for k3", k3);
fr.register(target, "no token");
fr.register(ksym, "symbol target", tok);
fr.register({ name: "temp" }, "dead target", { name: "dead token" });
fr.register({ name: "temp" }, "dead target, live token", k2);
var fr2 = new FinalizationRegistry(function cleanup() {});

class Cache extends WeakMap {
    lookup(k) { return this.has(k) ? "hit:" + this.get(k) : "miss"; }
}
var cache = new Cache([[k1, 1]]);

function report(label) {
    print("--", label);
    print("wm", wm.get(k1), wm.has(k2), wm.get(k2), wm.get(ksym), wm.get(k3) === k1,
          wm.has({}), wm instanceof WeakMap, Object.prototype.toString.call(wm));
    print("ws", ws.has(k1), ws.has(k2), ws.has(k3), ws.has(ksym), ws.has(Symbol("weak key")));
    print("wr", wr.deref() === target, wr.deref().name, wrDead.deref(), wrSym.deref() === ksym,
          wrFn.deref() === report, Object.prototype.toString.call(wr));
    print("fr", Object.prototype.toString.call(fr), typeof fr.register, fr2 instanceof FinalizationRegistry);
    print("cache", cache.lookup(k1), cache.lookup(k2), cache instanceof Cache);
}
report("initial");

Promise.resolve().then(() => {
    report("job 1");
    wm.set(k2, "updated");
    wm.delete(ksym);
    ws.delete(k1);
    ws.add(k2);
    cache.set(k2, 2);
    print("unregister k3", fr.unregister(k3));
    fr2.register(k2, "fr2 held", k2);
}).then(() => {
    report("job 2");
    print("unregister k3 again", fr.unregister(k3));
    print("unregister tok", fr.unregister(tok));
    print("unregister tok again", fr.unregister(tok));
    fr.register(k1, "registered after restore", tok);
    var k4 = { name: "k4" };
    wm.set(k4, "k4");
    print("k4", wm.get(k4));
});

function onResume() {
    report("resume");
    print("unregister tok", fr.unregister(tok));
    print("unregister fr2", fr2.unregister(k2), fr2.unregister(k2));
    print("bad token", (() => { try { fr.unregister(1); } catch (e) { return e.name; } })());
}
