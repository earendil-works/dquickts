/* Regressions for the bug fixes ported from quickjs-ng */
import * as std from "std";
import { assert, assertThrows, assertArrayEquals } from "./assert.js";

/* ng #1552: Iterator.prototype.constructor setter */
function test_iterator_proto_constructor()
{
    var desc = Object.getOwnPropertyDescriptor(Iterator.prototype, "constructor");
    var get = desc.get, set = desc.set, o;

    assert(get !== set, true);
    assert(get.length, 0);
    assert(set.length, 1);
    assert(get.call({}), Iterator);

    o = {};
    assert(set.call(o), undefined);
    desc = Object.getOwnPropertyDescriptor(o, "constructor");
    assert(desc.value, undefined);
    assert(desc.writable && desc.enumerable && desc.configurable, true);

    o = {};
    set.call(o, 0); /* not necessarily an object */
    assert(o.constructor, 0);

    assertThrows(TypeError, () => set.call(undefined, 0));
    assertThrows(TypeError, () => set.call(1, 0));
    assertThrows(TypeError, () => set.call(Iterator.prototype, 0));
    assert(Iterator.prototype.constructor, Iterator);

    o = {};
    Object.defineProperty(o, "constructor", { value: 1, writable: false,
                                              configurable: true });
    assertThrows(TypeError, () => set.call(o, 2));
    assert(o.constructor, 1);

    o = { constructor: 1 };
    set.call(o, 2);
    assert(o.constructor, 2);

    o = Object.preventExtensions({});
    assertThrows(TypeError, () => set.call(o, 0));

    o = Object.create(Iterator.prototype);
    o.constructor = 42;
    assert(Object.getOwnPropertyDescriptor(o, "constructor").value, 42);
    assert(Iterator.prototype.constructor, Iterator);
}

/* Iterator.prototype[Symbol.toStringTag] setter on a non extensible
   receiver */
function test_iterator_proto_tostringtag()
{
    var set = Object.getOwnPropertyDescriptor(Iterator.prototype,
                                              Symbol.toStringTag).set;
    var o = Object.preventExtensions({});
    assertThrows(TypeError, () => set.call(o, "x"));
    assert(Object.prototype.hasOwnProperty.call(o, Symbol.toStringTag), false);
}

/* ng #1591, #1598: regexp parser */
function test_regexp()
{
    function pat(n) {
        return "(?<g>a)" + "|x".repeat(n) + "|(?<g>c)";
    }
    function message(re) {
        try {
            new RegExp(re);
        } catch (e) {
            return e.message;
        }
        return undefined;
    }
    assert(new RegExp(pat(254)).test("a"), true);
    assert(message(pat(255)), "too many named groups");
    assert(message("(?<g>a)(?<g>b)"), "duplicate group name");

    /* ClassSetReservedPunctuator escapes in 'v' classes and \q{} */
    for (var c of "&-!#%,:;<=>@`~") {
        assert(new RegExp("[\\q{\\" + c + "}]", "v").test(c), true, c);
        assert(new RegExp("[\\" + c + "]", "v").test(c), true, c);
    }
    assertThrows(SyntaxError, () => new RegExp("\\%", "v"));
    assertThrows(SyntaxError, () => new RegExp("[\\&]", "u"));
    assertThrows(SyntaxError, () => new RegExp("[\\q{(}]", "v"));
    assertThrows(SyntaxError, () => new RegExp("[\\q{&&}]", "v"));
    assert(/[\q{a\-b}]/v.test("a-b"), true);
}

function test_typed_array()
{
    /* ng #1625: the value is converted before the index is checked */
    assertThrows(TypeError, () => new BigInt64Array().with());
    assertThrows(TypeError, () => new BigInt64Array().with(0, 1));
    assertThrows(RangeError, () => new BigInt64Array().with(0, 10n));
    assertThrows(RangeError, () => new Int8Array().with(0, "1"));

    /* ng #1690: content type mismatch even without elements */
    assertThrows(TypeError, () => new Int8Array(new BigInt64Array()));
    assertThrows(TypeError, () => new Int8Array(new BigUint64Array()));
    assertThrows(TypeError, () => new BigInt64Array(new Int8Array()));
    assertThrows(TypeError, () => new BigUint64Array(new Float64Array(1)));
    assert(new BigInt64Array(new BigUint64Array([1n])).toString(), "1");
}

/* ng #1626, #1628, #1609: proxy invariants */
function test_proxy()
{
    function inner(target, isExtensible) {
        return new Proxy(target, { isExtensible });
    }
    function trap_log(target, op) {
        var log = [];
        var p = new Proxy(target, {
            getOwnPropertyDescriptor(t, k) {
                log.push("gOPD");
                return Reflect.getOwnPropertyDescriptor(t, k);
            },
            isExtensible(t) {
                log.push("isExtensible");
                return Reflect.isExtensible(t);
            },
        });
        op(p);
        return log.join();
    }

    /* IsExtensible(target) goes through the target proxy */
    assertThrows(TypeError, () => Reflect.defineProperty(
        new Proxy(inner({}, 0), { defineProperty: () => true }), "x", {}));
    assertThrows(TypeError, () => Reflect.getOwnPropertyDescriptor(
        new Proxy(inner({ x: 1 }, 0), { getOwnPropertyDescriptor: () => undefined }), "x"));
    assertThrows(TypeError, () => Reflect.has(
        new Proxy(inner({ x: 1 }, 0), { has: () => false }), "x"));
    assertThrows(TypeError, () => Reflect.defineProperty(
        new Proxy(inner({}, () => false), { defineProperty: () => true }), "x", {}));
    assertThrows(RangeError, () => Reflect.has(
        new Proxy(inner({ x: 1 }, () => { throw new RangeError(); }),
                  { has: () => false }), "x"));
    assert(trap_log({ x: 1 }, (p) => Reflect.has(
        new Proxy(p, { has: () => false }), "x")), "gOPD,isExtensible");
    assert(trap_log({}, (p) => Reflect.has(
        new Proxy(p, { has: () => false }), "x")), "gOPD");
    assert(trap_log({}, (p) => Reflect.defineProperty(
        new Proxy(p, { defineProperty: () => true }), "x", { value: 1 })),
           "gOPD,isExtensible");

    /* the ownKeys trap result must be an object */
    for (var v of [0, "ab", true, undefined, null, 1n, Symbol("s")]) {
        var p = new Proxy({}, { ownKeys() { return v; } });
        assertThrows(TypeError, () => Reflect.ownKeys(p));
        assertThrows(TypeError, () => Object.keys(p));
        assertThrows(TypeError, () => { for (var k in p); });
    }
    assertArrayEquals(Reflect.ownKeys(new Proxy({}, {
        ownKeys: () => ({ length: 2, 0: "x", 1: "y" }) })), ["x", "y"]);

    /* a huge 'length' must not allocate the key table up front */
    for (var length of [0x20000000, 0xfffffffe, 0xffffffff]) {
        let p = new Proxy({}, { ownKeys() { return { length }; } });
        assertThrows(TypeError, () => Object.getOwnPropertyNames(p));
    }
    var keys = ["a", "b", Symbol("c")];
    var p = new Proxy({}, {
        ownKeys() {
            return new Proxy({}, {
                get(t, k) {
                    if (k === "length")
                        return 0xffffffff;
                    if (Number(k) < keys.length)
                        return keys[Number(k)];
                    throw new RangeError("stop");
                },
            });
        },
    });
    assertThrows(RangeError, () => Object.getOwnPropertyNames(p));
}

/* ng #1627: ToPropertyDescriptor propagates abrupt completions */
function test_to_property_descriptor()
{
    for (var key of ["get", "set", "enumerable", "value"]) {
        var sentinel = new Error(key);
        var caught;
        try {
            Object.defineProperty({}, "p", { get [key]() { throw sentinel; } });
        } catch (e) {
            caught = e;
        }
        assert(caught, sentinel, key);
    }
    /* HasProperty() failures too */
    var desc = new Proxy({}, { has() { throw new RangeError("has"); } });
    assertThrows(RangeError, () => Object.defineProperty({}, "p", desc));
    assertThrows(TypeError, () => Object.defineProperty({}, "p", { get: 1 }));
    var o = {};
    Object.defineProperty(o, "p", { get get() { return () => 42; } });
    assert(o.p, 42);
}

/* the JSON.stringify() gap can be a rope */
function test_json_rope_gap()
{
    var rope = "a".repeat(600) + "b".repeat(600);
    var expected = '{\n' + "a".repeat(10) + '"x": 1\n}';
    assert(JSON.stringify({x:1}, null, rope), expected);
    assert(JSON.stringify({x:1}, null, new String(rope)), expected);
    var wide = "é".repeat(600) + "è".repeat(600);
    assert(JSON.stringify({x:1}, null, wide),
           '{\n' + "é".repeat(10) + '"x": 1\n}');
}

/* computed access on null/undefined names the (primitive) key */
function test_null_property_message()
{
    var k = "x", e;
    try { null[k]; } catch (ex) { e = ex; }
    assert(e.message, "cannot read property 'x' of null");
    try { undefined[1]; } catch (ex) { e = ex; }
    assert(e.message, "cannot read property '1' of undefined");
}

/* ng #1623: Iterator.concat() return() completes the iterator */
function test_iterator_concat_return()
{
    var c, r, calls = 0;

    c = Iterator.concat([1]);
    c.next();
    c.next();
    r = c.return();
    assert(r.done, true);
    assert(r.value, undefined);

    /* the inner iterator has no return() method */
    c = Iterator.concat({ [Symbol.iterator]() {
        return { next() { return { done: false, value: 1 }; } };
    } });
    c.next();
    r = c.return();
    assert(r.done, true);
    assert(c.next().done, true);

    /* the inner return() result is ignored but must be an object */
    c = Iterator.concat({ [Symbol.iterator]() {
        return { next() { return { done: false, value: 1 }; },
                 return() { calls++; return { value: 42 }; } };
    } });
    c.next();
    r = c.return();
    assert(r.done, true);
    assert(r.value, undefined);
    assert(calls, 1);
    c = Iterator.concat({ [Symbol.iterator]() {
        return { next() { return { done: false, value: 1 }; },
                 return() { return 1; } };
    } });
    c.next();
    assertThrows(TypeError, () => c.return());
}

/* ng #1695: the error does not mention proxies */
function test_prevent_extensions_resizable()
{
    var ta = new Int8Array(new ArrayBuffer(16, { maxByteLength: 16 }));
    var e;
    try {
        Object.preventExtensions(ta);
    } catch (ex) {
        e = ex;
    }
    assert(e instanceof TypeError, true);
    assert(e.message, "cannot prevent extensions");
    assert(Reflect.preventExtensions(ta), false);
}

/* ng #1720: integer keys which are multiples of a power of two must not
   all fall in the same property hash bucket */
function test_property_hash()
{
    var o = {}, i, n = 4096, s = 0;
    for (i = 0; i < n; i++)
        o[i * 65536] = i;
    for (i = 0; i < n; i++)
        s += o[i * 65536];
    assert(s, n * (n - 1) / 2);
    for (i = 0; i < n; i += 2)
        delete o[i * 65536];
    assert(Object.keys(o).length, n / 2);
    assert(o[65536], 1);
    assert(o[0], undefined);
}

/* class fields named like contextual keywords */
function test_class_fields()
{
    class P {
        get;
        set;
        async;
        get = () => "123";
        async = () => "789";
        static() { return 42; }
    }
    var p = new P();
    assert(p.get(), "123");
    assert(p.set, undefined);
    assert(p.async(), "789");
    assert(p.static(), 42);
}

/* ng #1577, #1580: for-await does not call return() on normal completion
   and awaits its result on break or return */
async function test_for_await_close()
{
    var log = [];
    function iter(n, slow_return) {
        var i = 0;
        return { [Symbol.asyncIterator]() { return {
            next() {
                return Promise.resolve(i < n ? { value: i++, done: false } :
                                       { value: undefined, done: true });
            },
            return() {
                log.push("return");
                if (!slow_return)
                    return Promise.resolve({ done: true });
                return Promise.resolve().then(() => {
                    log.push("resolved");
                    return { done: true };
                });
            },
        }; } };
    }
    for await (var x of iter(2, false)) { }
    assert(log.join(), "");
    for await (var x of iter(5, true)) break;
    log.push("after");
    assert(log.join(), "return,resolved,after");
    log = [];
    await (async function() {
        for await (var x of iter(5, true))
            return;
    })();
    log.push("after");
    assert(log.join(), "return,resolved,after");
    log = [];
    outer: for (var y of [1]) {
        for await (var x of iter(5, true))
            break outer;
    }
    log.push("after");
    assert(log.join(), "return,resolved,after");

    /* the awaited result of return() must be an object */
    var e;
    try {
        for await (var x of { [Symbol.asyncIterator]() { return {
            next() { return Promise.resolve({ value: 1, done: false }); },
            return() { return Promise.resolve(1); } }; } })
            break;
    } catch (ex) {
        e = ex;
    }
    assert(e instanceof TypeError, true);

    /* return() of a sync iterator is not awaited in an async generator */
    log = [];
    var g = (async function*() {
        for (var x of { [Symbol.iterator]() { return {
            next() { return { value: 1, done: false }; },
            return() { log.push("return"); return { then() { log.push("then"); } }; } }; } })
            return 1;
    })();
    assert((await g.next()).value, 1);
    assert(log.join(), "return");
}

async function test_modules()
{
    var e1, e2, m1, m2, t1, t2, self;

    /* ng #1586: a re-export of a missing binding blames the source module
       and the name looked up there, like a direct import */
    e1 = await import("./fixture_reexport_missing.js").then(() => null, e => e);
    e2 = await import("./fixture_reexport_direct.js").then(() => null, e => e);
    assert(e1 instanceof SyntaxError, true);
    assert(e2 instanceof SyntaxError, true);
    assert(e1.message, e2.message);
    assert(e1.message.includes("fixture_reexport_source.js"), true);

    /* ng #1554: the internal promise of a throwing module is not
       reported as an unhandled rejection (qjs would exit with an error) */
    e1 = await import("./fixture_throwing_module.js").then(() => null, e => e);
    assert(e1.message, "thrown by the module");

    /* ng #1603: the module identity is keyed by the specifier and the
       import attributes */
    m1 = await import("./fixture_import_attributes.json", { with: { type: "json" } });
    m2 = await import("./fixture_import_attributes.json", { with: { type: "json" } });
    t1 = await import("./fixture_import_attributes.json", { with: { type: "text" } });
    t2 = await import("./fixture_import_attributes.json", { with: { type: "text" } });
    assert(m1.default.answer, 42);
    assert(m1 === m2, true);
    assert(typeof t1.default, "string");
    assert(JSON.parse(t1.default).answer, 42);
    assert(t1 === t2, true);
    /* a module can import its own source as text */
    self = await import("./test_ng_fixes.js", { with: { type: "text" } });
    assert(self.default.includes("test_modules"), true);
    assert(typeof import.meta.url, "string");
}

test_iterator_proto_constructor();
test_iterator_proto_tostringtag();
test_regexp();
test_typed_array();
test_proxy();
test_to_property_descriptor();
test_json_rope_gap();
test_class_fields();
test_null_property_message();
test_iterator_concat_return();
test_prevent_extensions_resizable();
test_property_hash();
await test_for_await_close();
await test_modules();
