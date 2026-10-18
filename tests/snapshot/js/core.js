// Core object model: every value created here must survive hibernation.
var counter = (function () {
    var n = 0;
    return { inc() { return ++n; }, get value() { return n; } };
})();
counter.inc();

var sym = Symbol("tag");
var anon = Symbol();
var reg = Symbol.for("registered");
var obj = { a: 1, "b c": "two", [sym]: 3, nested: { arr: [1, , 3, "x"] }, big: 12345678901234567890n };
obj.self = obj;
Object.defineProperty(obj, "hidden", { value: 42, enumerable: false });
Object.defineProperty(obj, "ro", { value: "ro", writable: false });
var frozen = Object.freeze({ f: 1 });
var sealed = Object.seal([1, 2]);
var slow = [];
slow[100] = "far";
slow.length = 200;
var arr = [1, 2.5, -0, NaN, Infinity, "s", null, undefined, true, obj];
arr.extra = "prop";

class Point {
    #x; #y;
    static count = 0;
    constructor(x, y) { this.#x = x; this.#y = y; Point.count++; }
    get x() { return this.#x; }
    #secret() { return this.#x * this.#y; }
    area() { return this.#secret(); }
    toString() { return `Point(${this.#x}, ${this.#y})`; }
}
class Point3 extends Point {
    constructor(x, y, z) { super(x, y); this.z = z; }
    toString() { return super.toString() + "+" + this.z; }
}
var p = new Point3(2, 3, 4);

function sloppyArgs(a, b) {
    arguments[0] = "changed";
    return function () { return [a, b, arguments.length]; };
}
var argsClosure = sloppyArgs("orig", "b");
var strictArgs = (function () { "use strict"; return arguments; })(1, 2, 3);
var bound = function (x, y) { return [this.tag, x, y]; }.bind({ tag: "bound" }, "first");
var re = /a(b+)c/gi;
re.lastIndex = 2;
var date = new Date(Date.UTC(2020, 1, 29, 12, 0, 0));
var boxed = [new Number(1), new String("str"), new Boolean(false), Object(sym), Object(10n)];
var json = JSON.parse('{"k":[1,{"z":null}]}');
let lexical = "let at top level";
const constant = { c: 1 };
var err = new TypeError("boom");
var protoLess = Object.create(null);
protoLess.k = "v";
var withAccessor = { _v: 1, get v() { return this._v; }, set v(x) { this._v = x * 2; } };
withAccessor.v = 5;
function* never() {}
var tpl = (s => s)`a${1}b`;

function report() {
    print("counter", counter.inc(), counter.value);
    print("obj", obj.a, obj["b c"], obj[sym], obj.self === obj, obj.nested.arr.length, 1 in obj.nested.arr, obj.big);
    print("hidden", obj.hidden, Object.keys(obj).join(","), Object.getOwnPropertyDescriptor(obj, "ro").writable);
    print("symbols", sym.toString(), anon.toString(), anon.description, reg === Symbol.for("registered"), Symbol.keyFor(reg));
    print("frozen", Object.isFrozen(frozen), Object.isSealed(sealed), Object.isExtensible(obj));
    print("slow", slow.length, slow[100], 50 in slow);
    print("arr", arr.length, arr.map(String).join("|"), Object.is(arr[2], -0), arr.extra);
    print("class", p.toString(), p.x, p.area(), p instanceof Point, Point.count, Object.getPrototypeOf(Point3) === Point);
    print("args", argsClosure().join(","), Array.from(strictArgs).join(","));
    print("bound", bound("second").join(","), bound.name, bound.length);
    print("regexp", re.lastIndex, re.exec("xxabbbc"), re.lastIndex, re.flags, re.source);
    print("date", date.toISOString(), date.getTime());
    print("boxed", boxed.map(x => typeof x + ":" + String(x.valueOf())).join(","));
    print("json", JSON.stringify(json), JSON.stringify({ arr: arr.slice(0, 9), nested: obj.nested }, (k, v) => typeof v === "bigint" ? "big" : v));
    print("lexical", lexical, constant.c, err.message, err instanceof TypeError, err.name);
    print("protoLess", Object.getPrototypeOf(protoLess), protoLess.k);
    print("accessor", withAccessor.v);
    print("generator fn", typeof never, never.constructor.name);
    print("template", tpl.raw.join("/"), Object.isFrozen(tpl));
    print("math", Math.floor(7.5), Math.max(1, 9, 3), Math.sqrt(16), typeof Math.random());
    print("string", "abc".padStart(5, "-"), "a-b-c".split("-").reverse().join(""), "ß".toUpperCase());
}
report();
// called by the host after the last hibernation
function onResume() {
    lexical = "updated";
    counter.inc();
    report();
}
