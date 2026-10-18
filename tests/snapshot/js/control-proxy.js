// Proxy objects (traps, function proxies, revocation) across hibernations.

var log = [];
var target = { a: 1, b: 2 };
var handler = {
    get(t, k, r) {
        log.push("get " + String(k));
        return k in t ? t[k] : "default:" + String(k);
    },
    set(t, k, v) {
        log.push("set " + String(k));
        t[k] = v * 10;
        return true;
    },
    has(t, k) {
        return typeof k === "string" && k.startsWith("a");
    },
    deleteProperty(t, k) {
        log.push("del " + String(k));
        delete t[k];
        return true;
    },
    ownKeys(t) {
        return [...Reflect.ownKeys(t), "virtual"];
    },
    getOwnPropertyDescriptor(t, k) {
        if (k === "virtual")
            return { value: "v", enumerable: true, configurable: true };
        return Reflect.getOwnPropertyDescriptor(t, k);
    },
};
var proxy = new Proxy(target, handler);
proxy.c = 3;

function add(x, y) { return x + y; }
var fnProxy = new Proxy(add, {
    apply(t, th, args) { return "applied:" + t(...args) + ":" + (th && th.tag); },
    construct(t, args, nt) { return { constructed: args, sameNewTarget: nt === fnProxy }; },
});
class K {
    constructor(v) { this.v = v; }
    get double() { return this.v * 2; }
}
var classProxy = new Proxy(K, {});
var arrowProxy = new Proxy(() => "arrow", {}); // not a constructor

var { proxy: revocable, revoke } = Proxy.revocable({ secret: 1 }, {});
var revokedEarly = Proxy.revocable(function () {}, {});
revokedEarly.revoke();
var later = Proxy.revocable({ x: 1 }, { get: (t, k) => "trap:" + String(k) });

var proxyProto = Object.create(proxy); // a proxy in the prototype chain
var nested = new Proxy(proxy, {});
var arrayProxy = new Proxy([1, 2, 3], {});
var counterHandler = { count: 0, get(t, k) { this.count++; return Reflect.get(t, k); } };
var counted = new Proxy({ n: 5 }, counterHandler);

function attempt(f) {
    try {
        return f();
    } catch (e) {
        return e.name + ": " + e.message;
    }
}

function report(tag) {
    log.length = 0;
    print(tag, "get", proxy.a, proxy.c, proxy.zzz, "in", "a" in proxy, "b" in proxy);
    proxy.d = 4;
    delete proxy.b;
    print(tag, "keys", Object.keys(proxy).join(","), JSON.stringify(target));
    print(tag, "log", log.join("|"));
    print(tag, "fn", typeof fnProxy, fnProxy(1, 2), fnProxy.call({ tag: "T" }, 3, 4), JSON.stringify(new fnProxy(5, 6)));
    const k = new classProxy(21);
    print(tag, "class", typeof classProxy, k instanceof K, k.double, attempt(() => new arrowProxy()), arrowProxy());
    print(tag, "revocable", attempt(() => revocable.secret), attempt(() => revokedEarly.proxy.x), attempt(() => typeof revokedEarly.proxy));
    print(tag, "later", attempt(() => later.proxy.anything));
    print(tag, "proto", proxyProto.a, proxyProto.missing, nested.c, Array.isArray(arrayProxy), arrayProxy.length);
    print(tag, "counted", counted.n, counted.n, counterHandler.count);
    print(tag, "json", JSON.stringify(proxy), Object.prototype.toString.call(arrayProxy));
}

report("start");
Promise.resolve().then(() => report("job"));

globalThis.onResume = function () {
    report("resume");
    revoke();
    later.revoke();
    print("after revoke", attempt(() => revocable.secret), attempt(() => later.proxy.anything), attempt(() => Object.keys(revocable)));
    revoke();
};
