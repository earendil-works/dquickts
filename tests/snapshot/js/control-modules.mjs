// Module graph across hibernations: static imports with live bindings,
// re-exports, namespaces, import.meta, top-level await spanning jobs and
// dynamic import() (some of them loaded after a restore).
import { counter, increment, label, meta, Thing, renamed } from "./control-modules/a.mjs";
import * as ns from "./control-modules/a.mjs";
import defB, { fromB, progress, nsA, aLabel, counter as bCounter } from "./control-modules/b.mjs";
import * as nsB from "./control-modules/b.mjs";
import { hostValue, hostTwice } from "host:native"; // C module of the test host
import * as hostNs from "host:native";

print("main start", import.meta.url, import.meta.main, counter, label, renamed, JSON.stringify(meta));
print("namespaces", Object.keys(ns).join(","), Object.keys(nsB).join(","), nsA === ns, ns[Symbol.toStringTag]);
print("b state", defB, progress, aLabel, bCounter);
print("C module", hostValue, hostTwice(hostValue), Object.keys(hostNs).join(","));
increment();
new Thing();
print("after increment", counter, ns.counter, nsB.counter, bCounter, Thing.created);

let dResolve;
globalThis.dGate = new Promise(r => { dResolve = r; });
const cPromise = import("./control-modules/c.mjs");
cPromise.then(c => print("c loaded", c.value));
await null;
print("after await", counter, progress);
dResolve("d-resolved");
const c = await cPromise;
print("dynamic", c.value, c.default(), c.readA(), Object.keys(c).join(","));
const badError = await import("./control-modules/bad.mjs").catch(e => e);
print("bad import", badError.name, badError.message, counter);
const missing = await import("./control-modules/missing.mjs").catch(e => e.name + ": " + e.message);
print("missing import", missing);
increment(2);
print("main end", counter, c.readA(), ns.counter);

globalThis.onResume = async function () {
    increment(5);
    print("resume", counter, ns.counter, c.readA(), fromB(), defB);
    const again = await import("./control-modules/c.mjs");
    print("same module", again === c, again.readA());
    const badAgain = await import("./control-modules/bad.mjs").catch(e => e);
    print("same error", badAgain === badError);
    const usesBad = await import("./control-modules/uses-bad.mjs").catch(e => e);
    print("uses bad", usesBad === badError);
    const host2 = await import("host:native");
    print("C module again", host2 === hostNs, hostTwice(host2.hostValue + 1));
    const a2 = await import("./control-modules/a.mjs");
    print("same namespace", a2 === ns, a2.counter);
    try {
        ns.counter = 1;
    } catch (e) {
        print("namespace write", e.name);
    }
};
