import * as std from "std";
import * as bjson from "bjson";

function assert(actual, expected, message) {
    if (arguments.length == 1)
        expected = true;

    if (actual === expected)
        return;

    if (actual !== null && expected !== null
    &&  typeof actual == 'object' && typeof expected == 'object'
    &&  actual.toString() === expected.toString())
        return;

    throw Error("assertion failed: got |" + actual + "|" +
                ", expected |" + expected + "|" +
                (message ? " (" + message + ")" : ""));
}

function toHex(a)
{
    var i, s = "", tab, v;
    tab = new Uint8Array(a);
    for(i = 0; i < tab.length; i++) {
        v = tab[i].toString(16);
        if (v.length < 2)
            v = "0" + v;
        if (i !== 0)
            s += " ";
        s += v;
    }
    return s;
}

function isArrayLike(a)
{
    return Array.isArray(a) ||
        (a instanceof Uint8ClampedArray) ||
        (a instanceof Uint8Array) ||
        (a instanceof Uint16Array) ||
        (a instanceof Uint32Array) ||
        (a instanceof Int8Array) ||
        (a instanceof Int16Array) ||
        (a instanceof Int32Array) ||
        (a instanceof Float16Array) ||
        (a instanceof Float32Array) ||
        (a instanceof Float64Array);
}

function toStr(a)
{
    var s, i, props, prop;

    switch(typeof(a)) {
    case "object":
        if (a === null)
            return "null";
        if (a instanceof Date) {
            s = "Date(" + toStr(a.valueOf()) + ")";
        } else if (a instanceof Number) {
            s = "Number(" + toStr(a.valueOf()) + ")";
        } else if (a instanceof String) {
            s = "String(" + toStr(a.valueOf()) + ")";
        } else if (a instanceof Boolean) {
            s = "Boolean(" + toStr(a.valueOf()) + ")";
        } else if (isArrayLike(a)) {
            s = "[";
            for(i = 0; i < a.length; i++) {
                if (i != 0)
                    s += ",";
                s += toStr(a[i]);
            }
            s += "]";
        } else {
            props = Object.keys(a);
            s = "{";
            for(i = 0; i < props.length; i++) {
                if (i != 0)
                    s += ",";
                prop = props[i];
                s += prop + ":" + toStr(a[prop]);
            }
            s += "}";
        }
        return s;
    case "undefined":
        return "undefined";
    case "string":
        return JSON.stringify(a);
    case "number":
        if (a == 0 && 1 / a < 0)
            return "-0";
        else
            return a.toString();
        break;
    default:
        return a.toString();
    }
}

function bjson_test(a)
{
    var buf, r, a_str, r_str;
    a_str = toStr(a);
    buf = bjson.write(a);
    if (0) {
        print(a_str, "->", toHex(buf));
    }
    r = bjson.read(buf, 0, buf.byteLength);
    r_str = toStr(r);
    if (a_str != r_str) {
        print(a_str);
        print(r_str);
        assert(false);
    }
}

function bjson_test_arraybuffer()
{
    var buf, array_buffer;

    array_buffer = new ArrayBuffer(4);
    assert(array_buffer.byteLength, 4);
    assert(array_buffer.maxByteLength, 4);
    assert(array_buffer.resizable, false);
    buf = bjson.write(array_buffer);
    array_buffer = bjson.read(buf, 0, buf.byteLength);
    assert(array_buffer.byteLength, 4);
    assert(array_buffer.maxByteLength, 4);
    assert(array_buffer.resizable, false);

    array_buffer = new ArrayBuffer(4, {maxByteLength: 4});
    assert(array_buffer.byteLength, 4);
    assert(array_buffer.maxByteLength, 4);
    assert(array_buffer.resizable, true);
    buf = bjson.write(array_buffer);
    array_buffer = bjson.read(buf, 0, buf.byteLength);
    assert(array_buffer.byteLength, 4);
    assert(array_buffer.maxByteLength, 4);
    assert(array_buffer.resizable, true);

    array_buffer = new ArrayBuffer(4, {maxByteLength: 8});
    assert(array_buffer.byteLength, 4);
    assert(array_buffer.maxByteLength, 8);
    assert(array_buffer.resizable, true);
    buf = bjson.write(array_buffer);
    array_buffer = bjson.read(buf, 0, buf.byteLength);
    assert(array_buffer.byteLength, 4);
    assert(array_buffer.maxByteLength, 8);
    assert(array_buffer.resizable, true);
}

/* test multiple references to an object including circular
   references */
function bjson_test_reference()
{
    var array, buf, i, n, array_buffer;
    n = 16;
    array = [];
    for(i = 0; i < n; i++)
        array[i] = {};
    array_buffer = new ArrayBuffer(n);
    for(i = 0; i < n; i++) {
        array[i].next = array[(i + 1) % n];
        array[i].idx = i;
        array[i].typed_array = new Uint8Array(array_buffer, i, 1);
    }
    /* the references need the REFERENCE flags */
    try {
        bjson.write(array);
        assert(false);
    } catch(e) {
        assert(e instanceof TypeError);
    }
    buf = bjson.write(array, bjson.WRITE_OBJ_REFERENCE);

    array = bjson.read(buf, 0, buf.byteLength, bjson.READ_OBJ_REFERENCE);

    /* check the result */
    for(i = 0; i < n; i++) {
        assert(array[i].next, array[(i + 1) % n]);
        assert(array[i].idx, i);
        assert(array[i].typed_array.buffer, array_buffer);
        assert(array[i].typed_array.length, 1);
        assert(array[i].typed_array.byteOffset, i);
    }
}

function bjson_test_regexp()
{
    var buf, r, re;

    bjson_test(/xyzzy/);
    bjson_test(/xyzzy/digu);

    buf = bjson.write(/(?<𝓓𝓸𝓰>dog)/);
    r = bjson.read(buf, 0, buf.byteLength);
    assert("sup dog".match(r).groups["𝓓𝓸𝓰"], "dog");

    /* the source and the flags are kept, lastIndex is reset */
    re = /[\p{L}--[a-z]]\//gimsvy;
    re.lastIndex = 3;
    buf = bjson.write(re);
    r = bjson.read(buf, 0, buf.byteLength);
    assert(r instanceof RegExp);
    assert(r.source, re.source);
    assert(r.flags, re.flags);
    assert(r.lastIndex, 0);
    assert(r.test("É/"), true);

    /* a non unicode pattern with a lone surrogate */
    re = new RegExp("\ud800|x");
    buf = bjson.write(re);
    r = bjson.read(buf, 0, buf.byteLength);
    assert(r.test("\ud800"), true);

    /* several references to the same RegExp */
    re = /a/;
    buf = bjson.write([re, re, {}], bjson.WRITE_OBJ_REFERENCE);
    r = bjson.read(buf, 0, buf.byteLength, bjson.READ_OBJ_REFERENCE);
    assert(r[0] === r[1]);
    assert(r[0] !== r[2]);
}

function bjson_test_map()
{
    var buf, r, xs, m, it;

    xs = [["key", "value"]];
    buf = bjson.write(new Map(xs));
    r = bjson.read(buf, 0, buf.byteLength);
    assert(r instanceof Map);
    assert([...r].toString(), xs.toString());

    /* insertion order, -0 key, deleted records still referenced by an
       iterator */
    m = new Map([[1, "a"], [-0, "b"], [NaN, "c"], ["d", [1, 2]]]);
    it = m.keys();
    it.next();
    m.delete(1);
    m.delete(NaN);
    m.set(3n, new Map());
    buf = bjson.write(m);
    r = bjson.read(buf, 0, buf.byteLength);
    assert(r.size, 3);
    assert(JSON.stringify([...r.keys()].map(String)),
           JSON.stringify(["0", "d", "3"]));
    assert(r.get(0), "b");
    assert(r.get("d").toString(), "1,2");
    assert(r.get(3n) instanceof Map);

    /* object keys, shared values and cycles need the reference flag */
    var k = {};
    m = new Map([[k, k]]);
    m.set("self", m);
    try {
        bjson.write(m);
        assert(false);
    } catch(e) {
        assert(e instanceof TypeError);
    }
    buf = bjson.write(m, bjson.WRITE_OBJ_REFERENCE);
    r = bjson.read(buf, 0, buf.byteLength, bjson.READ_OBJ_REFERENCE);
    assert(r.get("self") === r);
    var [k1, v1] = [...r][0];
    assert(k1 === v1);
    assert(r.get(k1) === k1);
}

function bjson_test_set()
{
    var buf, r, xs, s, o;

    xs = ["one", "two", "three"];
    buf = bjson.write(new Set(xs));
    r = bjson.read(buf, 0, buf.byteLength);
    assert(r instanceof Set);
    assert([...r].toString(), xs.toString());

    o = {x: 1};
    s = new Set([o, new Set([1])]);
    s.add(s);
    buf = bjson.write([s, o], bjson.WRITE_OBJ_REFERENCE);
    r = bjson.read(buf, 0, buf.byteLength, bjson.READ_OBJ_REFERENCE);
    assert(r[0].size, 3);
    assert(r[0].has(r[0]));
    assert(r[0].has(r[1]));
    assert(r[1].x, 1);

    /* the weak collections are not supported */
    for (var w of [new WeakMap(), new WeakSet()]) {
        try {
            bjson.write(w);
            assert(false);
        } catch(e) {
            assert(e instanceof TypeError);
        }
    }
}

function bjson_test_symbol()
{
    var buf, r, o, s, k;

    o = {[Symbol.toStringTag]: "42"};
    buf = bjson.write(o);
    r = bjson.read(buf, 0, buf.byteLength);
    assert(o.toString(), r.toString());

    o = Symbol('foo');
    buf = bjson.write(o);
    r = bjson.read(buf, 0, buf.byteLength);
    assert(o.toString(), r.toString());
    assert(o !== r);

    o = Symbol.for('foo');
    buf = bjson.write(o);
    r = bjson.read(buf, 0, buf.byteLength);
    assert(o === r);

    o = Symbol.toStringTag;
    buf = bjson.write(o);
    r = bjson.read(buf, 0, buf.byteLength);
    assert(o === r);

    /* also with the bytecode flag (the well-known symbols are not in
       the atom table) */
    buf = bjson.write([Symbol.iterator, Symbol.for("bar")], bjson.WRITE_OBJ_BYTECODE);
    r = bjson.read(buf, 0, buf.byteLength, bjson.READ_OBJ_BYTECODE);
    assert(r[0] === Symbol.iterator);
    assert(r[1] === Symbol.for("bar"));

    /* the description is kept, including undefined */
    buf = bjson.write([Symbol(), Symbol(""), Symbol("1")]);
    r = bjson.read(buf, 0, buf.byteLength);
    assert(r[0].description, undefined);
    assert(r[1].description, "");
    assert(r[2].description, "1");
    assert(Symbol.keyFor(r[2]), undefined);

    /* a symbol keeps its identity inside a payload */
    s = Symbol("s");
    k = Symbol.for("k");
    o = {[s]: s, [k]: 1, a: [s, k]};
    buf = bjson.write(o);
    r = bjson.read(buf, 0, buf.byteLength);
    var syms = Object.getOwnPropertySymbols(r);
    assert(syms.length, 2);
    assert(syms[0] !== s);
    assert(r[syms[0]] === syms[0]);
    assert(r.a[0] === syms[0]);
    assert(syms[1] === k);
    assert(r.a[1] === k);

    /* the private fields are not written */
    class A { #x = 1; y = 2; }
    bjson_test(new A());
}

function bjson_test_bytecode()
{
    var buf, o, r, e, i;

    o = std.evalScript(";(function f(o){ return o.i })", {compile_only: true});
    buf = bjson.write(o, bjson.WRITE_OBJ_BYTECODE);
    try {
        bjson.read(buf, 0, buf.byteLength);
    } catch (_e) {
        e = _e;
    }
    assert(String(e), "SyntaxError: no bytecode allowed");

    o = bjson.read(buf, 0, buf.byteLength, bjson.READ_OBJ_BYTECODE);
    assert(String(o), "[function bytecode]");
    o = std.evalScript(o, {eval_function: true});
    for (i = 0; i < 42; i++) o({i}); // exercise o.i IC
    assert(o({i: 7}), 7);

    /* a module */
    o = std.evalScript("export var x = 1; globalThis.bjson_module_x = x + 1;",
                       {compile_only: true, compile_module: true});
    buf = bjson.write(o, bjson.WRITE_OBJ_BYTECODE);
    o = bjson.read(buf, 0, buf.byteLength, bjson.READ_OBJ_BYTECODE);
    r = std.evalScript(o, {eval_module: true});
    assert(r instanceof Promise);
    assert(globalThis.bjson_module_x, 2);
    try {
        std.evalScript("1", {eval_module: true});
        assert(false);
    } catch(e) {
        assert(e instanceof TypeError);
    }
}

/* arbitrary inputs (with the check of the checksum disabled) must not
   crash the reader */
function bjson_test_fuzz()
{
    var corpus = [
        ["HP////8QAAAAAARg"],
        ["HP/////m5uaCLQ=="],
        ["HP////8AEQATBgYGBgYGBgYGBgb/////EAARAC8R/78vEf+/"],
        ["HP////8ACH8ACv////9//////////////////////////////9//AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABgAAAAAAAAAAAAAA+fn5+fn5+fn5+fn5AAAAAAAGAKs="],
        ["HP////8ADgAAABQA", bjson.READ_OBJ_REFERENCE],
        ["HP////8ADAAAAAAAAAAAAAAAAICAgIAEAAAAAA==", bjson.READ_OBJ_BYTECODE],
    ];
    for (var [input, flags] of corpus) {
        var tab = Uint8Array.fromBase64(input);
        tab[0] = 7; /* current version */
        try {
            bjson.read(tab.buffer, 0, tab.byteLength, flags);
        } catch (e) {
            if (/invalid version/.test(e.message)) throw e; // corpus needs update
            if (/checksum error/.test(e.message)) throw e;
        }
    }
}

/* invalid inputs with the check of the checksum disabled */
function bjson_test_invalid()
{
    var tests = [
        /* well-known symbol entry with a string atom */
        [[0x01, 0x00, 0x22, 0x17, 0x02], "invalid well-known symbol"],
        /* bad atom type */
        [[0x01, 0x05, 0x02, 0x61, 0x17, 0x02], "invalid atom type"],
        /* symbol tag with a string atom */
        [[0x01, 0x01, 0x02, 0x61, 0x17, 0x02], "invalid symbol"],
        /* regexp with unknown flags */
        [[0x00, 0x14, 0x02, 0x61, 0xff, 0x01], "invalid regular expression flags"],
        /* regexp with a bad pattern */
        [[0x00, 0x14, 0x02, 0x28, 0x00], "expecting ')'"],
        /* too many atoms */
        [[0x10, 0x01], "invalid atom count"],
    ];
    for (var [bytes, msg] of tests) {
        var tab = new Uint8Array([7, 0xff, 0xff, 0xff, 0xff, ...bytes]);
        try {
            bjson.read(tab.buffer, 0, tab.length);
            assert(false);
        } catch (e) {
            assert(e instanceof SyntaxError, true, e);
            assert(e.message.startsWith(msg), true, e.message);
        }
    }
}

function bjson_test_csum()
{
    var buf = bjson.write("fortytwo");
    var tab = new Uint8Array(buf);
    for (var i = 5; i < tab.length; i++) {
        for (var k = 0; k < 256; k++) {
            var t = tab[i];
            if (t == k)
                continue;
            tab[i] = k;
            var caught = false;
            try {
                bjson.read(buf, 0, buf.byteLength);
            } catch (e) {
                caught = /checksum error/.test(e.message);
            }
            assert(caught);
            tab[i] = t;
        }
    }

    /* a payload without checksum (written before it was added) */
    tab = new Uint8Array([7, 0, 7, 6, 0x61, 0x62, 0x63]);
    try {
        bjson.read(tab.buffer, 0, tab.length);
        assert(false);
    } catch (e) {
        assert(e instanceof SyntaxError);
        assert(e.message, "checksum error");
    }
}

function bjson_test_atom()
{
    var o = {return:1, with:2, Map:3}; // Map tests LEB128 encoding
    var b = bjson.write(o);
    var r = bjson.read(b, 0, b.byteLength);
    assert(JSON.stringify(Object.entries(r)),
           JSON.stringify(Object.entries(o)));
}

function bjson_test_range()
{
    var buf, r, tab, i;

    assert(bjson.READ_OBJ_BYTECODE, 1);
    assert(bjson.WRITE_OBJ_BYTECODE, 1);
    assert(bjson.READ_OBJ_REFERENCE, 8);
    assert(bjson.WRITE_OBJ_REFERENCE, 8);

    /* 'pos' and 'len' select the bytes to read */
    buf = bjson.write("abc");
    tab = new Uint8Array(buf.byteLength + 3);
    tab.set(new Uint8Array(buf), 2);
    r = bjson.read(tab.buffer, 2, buf.byteLength);
    assert(r, "abc");
    for (var [pos, len] of [[2, buf.byteLength + 2], [tab.length + 1, 0]]) {
        try {
            bjson.read(tab.buffer, pos, len);
            assert(false);
        } catch(e) {
            assert(e instanceof RangeError);
        }
    }
    try {
        bjson.read(new Uint8Array(buf), 0, buf.byteLength);
        assert(false);
    } catch(e) {
        assert(e instanceof TypeError);
    }
    /* a truncated input is detected */
    for (i = 0; i < buf.byteLength; i++) {
        try {
            bjson.read(buf, 0, i);
            assert(false);
        } catch(e) {
            assert(e instanceof SyntaxError, true, e);
        }
    }

    /* the SharedArrayBuffers can only be transferred to the workers:
       the SAB flag (4) is ignored */
    var caught = false;
    try {
        bjson.write(new SharedArrayBuffer(4), 4);
    } catch(e) {
        caught = true;
    }
    assert(caught, true, "SharedArrayBuffer written");
}

function bjson_test_all()
{
    var obj;

    bjson_test({x:1, y:2, if:3});

    bjson_test([1, 2, 3]);

    /* array with holes */
    bjson_test([1, , 2, , 3]); 

    /* fast array with hole */
    obj = new Array(5);
    obj[0] = 1;
    obj[1] = 2;
    bjson_test(obj);

    bjson_test([1.0, "aa", true, false, undefined, null, NaN, -Infinity, -0.0]);
    if (typeof BigInt !== "undefined") {
        bjson_test([BigInt("1"), -BigInt("0x123456789"),
               BigInt("0x123456789abcdef123456789abcdef")]);
    }
    bjson_test([new Date(1234), new String("abc"), new Number(-12.1), new Boolean(true)]);

    bjson_test(new Int32Array([123123, 222111, -32222]));
    bjson_test(new Float16Array([1024, 1024.5]));
    bjson_test(new Float64Array([123123, 222111.5]));

    /* tested with a circular reference */
    obj = {};
    obj.x = obj;
    try {
        bjson.write(obj);
        assert(false);
    } catch(e) {
        assert(e instanceof TypeError);
    }

    bjson_test_arraybuffer();
    bjson_test_reference();
    bjson_test_regexp();
    bjson_test_map();
    bjson_test_set();
    bjson_test_symbol();
    bjson_test_bytecode();
    bjson_test_fuzz();
    bjson_test_invalid();
    bjson_test_csum();
    bjson_test_atom();
    bjson_test_range();
}

bjson_test_all();
