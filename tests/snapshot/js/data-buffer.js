// ArrayBuffer, SharedArrayBuffer, every typed array type and DataView:
// contents, views sharing a buffer, resizable buffers with length-tracking
// views, detached buffers, immutable buffers.
function fmt(v) {
    if (typeof v === "bigint") return v + "n";
    if (Object.is(v, -0)) return "-0";
    return String(v);
}
function show(label, ta) {
    print(label, ta.constructor.name, ta.length, ta.byteOffset, ta.byteLength,
          ta.buffer.byteLength, Array.from(ta, fmt).join(","));
}

var all = [
    new Uint8ClampedArray([0, 255, 300, -5, 1.5, 2.5]),
    new Int8Array([-128, 127, 200, -1]),
    new Uint8Array([0, 1, 254, 255, 256]),
    new Int16Array([-32768, 32767, 40000]),
    new Uint16Array([0, 65535, 65536, -1]),
    new Int32Array([-2147483648, 2147483647, 3000000000]),
    new Uint32Array([0, 4294967295, -1, 1.9]),
    new BigInt64Array([-(2n ** 63n), 2n ** 63n - 1n, 2n ** 64n + 5n]),
    new BigUint64Array([0n, 2n ** 64n - 1n, -1n]),
    new Float16Array([1.5, -0, NaN, 65504, 1e-7, 70000, -Infinity]),
    new Float32Array([Math.PI, -0, NaN, Infinity, 1e-45, 16777217]),
    new Float64Array([Math.PI, -0, NaN, -Infinity, 5e-324, 2 ** 53 + 1]),
];
all[0].extra = "own property";

// views sharing one buffer
var shared = new ArrayBuffer(16);
var u8 = new Uint8Array(shared);
var i16 = new Int16Array(shared, 2, 3);
var f64 = new Float64Array(shared, 8, 1);
var sub = u8.subarray(4, 12);
var subsub = sub.subarray(2);
var dv = new DataView(shared, 1, 12);
var dvAll = new DataView(shared);
for (var i = 0; i < 16; i++) u8[i] = i * 17;
var copy = u8.slice(0, 4);
var empty = new Uint8Array(0);
var emptyAt = new Uint8Array(shared, 16);
var large = new Uint32Array(20000);
for (var i = 0; i < large.length; i++) large[i] = Math.imul(i, 2654435761) >>> 0;

// resizable buffer with length-tracking views
var rab = new ArrayBuffer(4, { maxByteLength: 32 });
var track = new Uint8Array(rab);
var track16 = new Uint16Array(rab, 2);
var fixed = new Uint8Array(rab, 0, 4);
var dvTrack = new DataView(rab, 1);
var dvFixed = new DataView(rab, 0, 2);
rab.resize(8);
track.set([1, 2, 3, 4, 5, 6, 7, 8]);
var rabShrunk = new ArrayBuffer(8, { maxByteLength: 8 });
var oob = new Uint8Array(rabShrunk, 4, 4);
rabShrunk.resize(2);                     // oob is out of bounds

// detached buffers
var src = new ArrayBuffer(8);
var srcView = new Uint8Array(src);
var srcDv = new DataView(src);
srcView.set([9, 8, 7, 6, 5, 4, 3, 2]);
var moved = src.transfer();
var movedView = new Uint8Array(moved);
var rsrc = new ArrayBuffer(4, { maxByteLength: 12 });
new Uint8Array(rsrc).set([1, 2, 3, 4]);
var rmoved = rsrc.transfer(6);
var fixedMoved = new ArrayBuffer(3, { maxByteLength: 10 }).transferToFixedLength();

// SharedArrayBuffer (memory private to the runtime)
var sab = new SharedArrayBuffer(8);
var sabView = new Int32Array(sab);
sabView[1] = 42;
var growable = new SharedArrayBuffer(4, { maxByteLength: 16 });
new Uint8Array(growable).set([4, 3, 2, 1]);

// immutable buffers; 'toFreeze' becomes immutable in a later job
var imm = new Uint8Array([10, 20, 30, 40]).buffer.transferToImmutable(6);
var immView = new Int16Array(imm, 2, 2);
var immDv = new DataView(imm);
var immSlice = shared.sliceToImmutable(1, 4);
var toFreeze = new ArrayBuffer(3);
var toFreezeView = new Uint8Array(toFreeze);
toFreezeView.set([7, 8, 9]);
var frozen = null;

function attempt(f) {
    try { return String(f()); } catch (e) { return e.name; }
}

function report(label) {
    print("--", label);
    all.forEach((ta, i) => show("t" + i, ta));
    print("extra", all[0].extra, Object.keys(all[0]).join(","));
    show("u8", u8); show("i16", i16); show("f64", f64); show("sub", sub); show("subsub", subsub);
    show("copy", copy); show("empty", empty); show("emptyAt", emptyAt);
    print("same buffer", u8.buffer === shared, i16.buffer === shared, sub.buffer === shared,
          dv.buffer === shared, copy.buffer === shared);
    print("dv", dv.byteOffset, dv.byteLength, dv.getUint8(0), dv.getUint16(1), dv.getUint16(1, true),
          dv.getInt32(3), dv.getInt32(3, true), dv.getFloat32(4, true), fmt(dv.getBigUint64(4)),
          fmt(dv.getBigInt64(4, true)), dv.getFloat64(2));
    var sum = 0;
    for (var i = 0; i < large.length; i++) sum = (sum + large[i]) % 1000000007;
    print("large", large.length, sum, large[12345]);
    print("rab", rab.detached, rab.byteLength, rab.maxByteLength, rab.resizable, track.length,
          track16.length, fixed.length, attempt(() => dvTrack.byteLength),
          attempt(() => dvFixed.byteLength), attempt(() => track.join(",")),
          attempt(() => track16.join(",")), attempt(() => fixed.join(",")));
    print("oob", oob.length, oob.byteOffset, oob.byteLength, rabShrunk.byteLength, attempt(() => oob.fill(1)));
    print("detached", src.detached, src.byteLength, src.resizable, srcView.length, srcView[0],
          attempt(() => srcDv.getUint8(0)), attempt(() => new Uint8Array(src)), attempt(() => src.slice(0)));
    print("moved", moved.detached, moved.byteLength, movedView.join(","));
    print("rmoved", rsrc.detached, rsrc.maxByteLength, rsrc.resizable, rmoved.resizable,
          rmoved.byteLength, rmoved.maxByteLength, new Uint8Array(rmoved).join(","));
    print("fixedMoved", fixedMoved.resizable, fixedMoved.byteLength, fixedMoved.maxByteLength);
    print("sab", sab instanceof SharedArrayBuffer, sab.byteLength, sabView.join(","),
          growable.growable, growable.byteLength, growable.maxByteLength,
          new Uint8Array(growable).join(","), Object.prototype.toString.call(sab));
    var d = Object.getOwnPropertyDescriptor(immView, "0");
    print("imm", imm.immutable, imm.resizable, imm.byteLength, new Uint8Array(imm).join(","),
          immView.join(","), immDv.getUint16(0, true), d.writable, d.configurable,
          Reflect.set(immView, 0, 1), Reflect.set(immView, 5, 1),
          attempt(() => immView.fill(0)), attempt(() => immDv.setUint8(0, 1)),
          attempt(() => imm.transfer()), attempt(() => imm.resize(0)), imm.detached);
    print("immSlice", immSlice.immutable, new Uint8Array(immSlice).join(","), shared.immutable);
    print("frozen", toFreeze.detached, toFreeze.immutable, toFreezeView.length,
          frozen && frozen.immutable, frozen && new Uint8Array(frozen).join(","),
          frozen && Reflect.set(new Uint8Array(frozen), 0, 0));
}
report("initial");

Promise.resolve().then(() => {
    report("job 1");
    // writes through one view are seen through the others
    i16[0] = -2;
    sub[0] = 0xaa;
    subsub[1] = 0xbb;
    dv.setUint32(8, 0x01020304);
    dv.setUint32(4, 0x01020304, true);
    f64[0] = -0;
    dvAll.setFloat32(12, 1.5);
    sabView[0] = -7;
    frozen = toFreeze.transferToImmutable(4);
    all.forEach(ta => { ta[0] = ta[ta.length - 1]; });
    rab.resize(16);
    track.fill(9, 8);
    srcView[0] = 1;                       // ignored: detached
}).then(() => {
    report("job 2");
    rab.resize(3);                        // track16 and dvFixed survive, fixed goes out of bounds
    print("after shrink", track.length, track16.length, fixed.length, dvTrack.byteLength,
          attempt(() => dvFixed.getUint16(0)), attempt(() => fixed[0]));
    var dst = rab.transfer();             // detaches rab and every view on it
    print("after transfer", rab.detached, track.length, track16.length,
          attempt(() => dvTrack.byteLength), dst.byteLength, dst.resizable);
    rabShrunk.resize(8);                  // oob is back in bounds
    oob.fill(5);
    print("oob back", oob.length, new Uint8Array(rabShrunk).join(","));
});

function onResume() {
    report("resume");
    moved.resize ? print("moved resize", attempt(() => moved.resize(4))) : 0;
    var t = moved.transfer(2);
    print("transfer again", moved.detached, movedView.length, new Uint8Array(t).join(","));
}
