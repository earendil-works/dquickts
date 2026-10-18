// Weak containers whose targets are already dead when hibernating: the
// restored runtime must still clean them up at its next GC, which an
// allocation burst forces in both runs. FinalizationRegistry cleanup jobs
// queued by that GC are themselves hibernated.
var log = [];
var fr = new FinalizationRegistry(held => log.push(held));
fr.register({ name: "temp" }, "dead target");
fr.register({ name: "temp" }, "dead target and token", { name: "temp token" });
fr.register(Symbol("temp"), "dead symbol target");
var keep = { name: "keep" };
fr.register(keep, "live target", keep);
var wrDead = new WeakRef({ name: "temp" });
var wrLive = new WeakRef(keep);
var wm = new WeakMap([[{ name: "temp" }, "dead key"], [keep, "live key"]]);
var ws = new WeakSet([{ name: "temp" }, keep, Symbol("temp")]);

function pressure() {
    var hold = [];
    for (var i = 0; i < 100000; i++)
        hold.push({ i: i, a: [i, i + 1] });
    return hold.length;
}

function report(label) {
    print(label, log.slice().sort().join(","), wrDead.deref(), wrLive.deref() === keep,
          wm.get(keep), ws.has(keep));
}

Promise.resolve().then(() => {
    print("pressure", pressure());
}).then(() => {
    print("pressure", pressure());
}).then(() => {
    report("after gc");
    print("unregister", fr.unregister(keep), fr.unregister(keep));
});

function onResume() {
    report("resume");
}
