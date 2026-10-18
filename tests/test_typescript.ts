// TypeScript support (JS_EVAL_FLAG_TYPESCRIPT): run by qjs as TypeScript
// because of the .ts extension.
import * as std from "std";
import { assert, assertThrows } from "./assert.js";

/* evaluate 'src' as a TypeScript (or JavaScript) script */
function evalTS(src: string, typescript: boolean = true): unknown {
    return std.evalScript(src, { typescript });
}

function assertSyntaxError(src: string, typescript: boolean = true): void {
    assertThrows(SyntaxError, () => evalTS(src, typescript));
}

function test_type_erasure() {
    let a: number = 1;
    let b!: string;
    const c: readonly number[] = [1, 2];
    let d: [x: number, y?: string, ...rest: boolean[]] = [1];
    let fn: (x: number, ...r: string[]) => void = () => {};
    let ctor: abstract new (...args: unknown[]) => object;
    let sym: unique symbol;
    function add(x: number, y?: number, ...rest: number[]): number {
        return x + (y ?? 0) + rest.length;
    }
    function withThis(this: { k: number }, n: number): number {
        return this.k + n;
    }
    function defaults({ p, q }: { p: number; q?: number } = { p: 1 },
                      [r]: [number] = [2]): number {
        return p + (q ?? 0) + r;
    }
    const arrow = (x: number, y: string = "y"): string => x + y;
    const asyncArrow = async <T,>(x: T): Promise<T> => x;
    const generic = <T extends object = {}>(x: T): T => x;
    const constGeneric = <const T extends readonly unknown[]>(x: T): T => x;
    assert(a + add(1, 2, 3, 4) + c.length + d.length, 9);
    assert(b, undefined);
    assert(withThis.call({ k: 1 }, 2), 3);
    assert(withThis.length, 1, "'this' is not a parameter");
    assert(defaults(), 3);
    assert(defaults({ p: 1, q: 2 }, [3]), 6);
    assert(arrow(1), "1y");
    assert(typeof asyncArrow(1).then, "function");
    assert(generic({ v: 2 }).v, 2);
    assert(constGeneric([1, 2] as const).length, 2);
    assert(typeof fn + typeof ctor + typeof sym, "functionundefinedundefined");

    /* catch binding, for loops, object literal methods */
    try {
        throw 1;
    } catch (e: unknown) {
        assert(e, 1);
    }
    let sum = 0;
    for (let i: number = 0; i < 3; i++)
        sum += i;
    for (const [k, v] of [["x", 1]] as [string, number][])
        sum += v;
    assert(sum, 4);
    const obj = {
        m<T>(x: T): T { return x; },
        get g(): number { return 1; },
        set s(v: number) {},
        async am<T>(): Promise<void> {},
    };
    assert(obj.m(2) + obj.g, 3);

    /* type predicates and assertion functions */
    function isString(x: unknown): x is string { return typeof x === "string"; }
    function assertNum(x: unknown): asserts x is number {}
    function assertThis(this: object): asserts this {}
    assert(isString("s"), true);
    assertNum(1);

    /* type declarations emit nothing */
    type Alias<in out T, const U = T> = T | U;
    interface Iface<T> extends Object { a?: T; readonly b: number; [k: string]: unknown; m?(): void; new (x: number): Iface<T>; <U>(u: U): U }
    declare var declaredVar: number;
    declare let declaredLet: string, declaredLet2: number;
    declare const declaredConst: number;
    declare function declaredFn(x: number): void;
    declare class DeclaredClass<T> extends Object implements Iface<T> { m(): void; }
    declare enum DeclaredEnum { A }
    declare const enum DeclaredConstEnum { A }
    declare namespace DeclaredNS { const x: number; }
    declare module DeclaredModule { }
    declare global { interface GlobalIface {} }
    assert(typeof declaredVar + typeof DeclaredClass + typeof DeclaredEnum +
           typeof DeclaredNS, "undefinedundefinedundefinedundefined");
    assertThrows(ReferenceError, () => declaredConst);
}

/* the type grammar: the declarations must be skipped entirely */
function test_type_grammar() {
    type Prim = any | unknown | never | void | null | undefined | object | number | bigint | string | boolean | symbol;
    type Lit = 1 | -1 | 1n | -2n | "s" | `t` | true | false;
    type Tpl<T extends string> = `a${T}b${Uppercase<T>}c`;
    type Fn = (a: number, b?: string, ...c: number[]) => void;
    type GenFn = <T>(x: T) => T;
    type Ctor = new <T>(x: T) => object;
    type AbsCtor = abstract new () => object;
    type Obj = { a: number; b?: string; readonly c: boolean; -readonly [K in "d"]-?: number };
    type Mapped<T> = { +readonly [K in keyof T as `get${Capitalize<string & K>}`]+?: () => T[K] };
    type Cond<T> = T extends (infer U extends string)[] ? U : T extends Promise<infer V> ? V : never;
    type Indexed = Obj["a"] | Obj[keyof Obj] | string[][number];
    type Tuple = [first: number, second?: string, ...rest: boolean[]] | readonly [number, string];
    type Q = typeof globalThis.Math.PI | keyof typeof Math;
    /* import types: covered here for the type skipper only */
    type Imp = typeof import("./assert.js") | import("./assert.js").Foo<number>;
    type Pred = (x: unknown) => x is string;
    type Asserts = (x: unknown) => asserts x is string;
    type ThisPred = { isFoo(): this is Obj };
    type Nested = Array<Array<Map<string, Set<number>>>>;
    type Unique = { readonly k: unique symbol };
    type Parens = (string | number)[] | (() => void) | ((x: number) => string)[];
    type Leading = | "a" | "b";
    type LeadingAnd = & { a: 1 } & { b: 2 };
    type Accessors = { get x(): number; set x(v: number) };
    let prims: Prim[] = [], lits: Lit[] = [], t: Tpl<"x"> | undefined, f: Fn | GenFn | Ctor | AbsCtor | undefined;
    let n: Nested = [[new Map()]];
    assert(n[0].length + prims.length + lits.length, 1);
}

function test_expressions() {
    function id<T>(x: T): T { return x; }
    const a = 1, b = 2, c = 3;

    /* generic call vs comparison */
    assert(id<number>(4), 4);
    assert(a < b > c, false, "(a < b) > c");
    assert(a < b >= c, false, "(a < b) >= c");
    assert(a < (b > c), false);
    const instantiated = id<string>;
    assert(instantiated("s"), "s");
    assert(new Map<string, number>([["k", 1]]).get("k"), 1);
    assert(new Array<number>().length, 0);
    const tag = (s: TemplateStringsArray, ...v: number[]) => s.raw[0] + v.length;
    assert(tag<number>`x${1}`, "x1");
    const nested = new Map<string, Array<number>>();
    assert(nested.size, 0);

    /* as, satisfies, non-null, angle bracket assertions */
    assert((1 + 2 as number) * 2, 6, "'as' binds tighter than '*' operands");
    assert(a as number < b, true, "'as' then '<'");
    assert(b as number * 3 + 1, 7, "(b as number) * 3 + 1");
    assert(b as number ** 3 ** 2, 512, "(b as number) ** (3 ** 2)");
    assert(1 + b satisfies number ** 2 - 1, 8, "((1 + b) satisfies number) ** 2 - 1");
    const cfg = { port: 80 } satisfies { port: number };
    assert(cfg.port, 80);
    const arr = [1, 2] as const;
    assert(arr.length, 2);
    const o: { p?: { q: number } } = { p: { q: 5 } };
    assert(o.p!.q, 5);
    assert(o.p!["q"]!, 5);
    let v = <number><unknown>"abc";
    assert(v, "abc");
    let x = 1;
    (x as number) = 2;
    (<number>x)++;
    x! += 1;
    assert(x, 4);
    const u = { k: 1 } as unknown as { k: number };
    assert(u.k, 1);

    /* arrow return types, in conditionals */
    const t = true;
    const f1 = (n: number): number => n + 1;
    const f2 = async (n: number): Promise<number> => n;
    const f3 = (n: number): { r: number } => ({ r: n });
    const f4 = (): (() => number) => () => 7;
    assert(f1(1) + f3(2).r + f4()(), 11);
    assert(typeof f2, "function");
    let y = 10, z = 20;
    /* 'x ? (y) : z => w' is a conditional: the arrow body is not followed by ':' */
    const c1 = t ? (y) : z => z;
    assert(c1, 10);
    const c2 = !t ? (y) : (z) => z;
    assert(typeof c2, "function");
    /* here the arrow has a return type */
    const c3 = t ? (n: number): number => n * 2 : null;
    assert(c3!(4), 8);
    const c4 = t ? (n): string => "r" + n : (n) => n;
    assert(c4(1), "r1");
    const c5 = t ? (a) : (b);
    assert(c5, 1);
}

function test_asi_contextual_keywords() {
    /* contextual keywords are identifiers where JavaScript allows them */
    var type = 1, declare = 2, abstract = 3, namespace = 4, module = 5;
    var readonly = 6, is = 7, asserts = 8, satisfies = 9, keyof = 10, infer = 11, global = 12;
    assert(type + declare + abstract + namespace + module + readonly + is +
           asserts + satisfies + keyof + infer + global, 78);
    /* a line break ends the statement as with tsc */
    let r: number[] = [];
    type
    Foo = 1;
    assert(Foo, 1);
    declare
    const fromDeclare = 2;
    assert(fromDeclare, 2);
    namespace
    Bar = 3;
    assert(Bar, 3);
    abstract
    class AfterAbstract {}
    assert(typeof AfterAbstract, "function");
    /* no line break before 'as' (tsc), but the type may follow one */
    let k = type as
        number;
    assert(k, 1);
    /* labels */
    type: for (;;) { break type; }
    declare: { r.push(1); }
    assert(r.length, 1);
    /* members named like modifiers */
    class M {
        readonly = 1;
        declare() { return 2; }
        abstract?: number;
        static public = 3;
        get<T>(x: T) { return x; }
        set?: number;
        is = 4;
    }
    const m = new M();
    assert(m.readonly + m.declare() + M.public + m.get(4) + m.is, 14);
    var Foo, Bar;
}

function test_classes() {
    abstract class Shape {
        abstract area(): number;
        abstract readonly kind: string;
        toString(): string { return this.kind + this.area(); }
    }
    class Square extends Shape implements Iterable<number> {
        readonly kind = "sq";
        [key: string]: unknown;
        declare extra: number;
        static readonly count: number = 0;
        private static secret?: string;
        protected optional?: number;
        definite!: number;
        constructor(private side: number) { super(); }
        area(): number { return this.side ** 2; }
        public override toString(): string { return "[" + super.toString() + "]"; }
        *[Symbol.iterator](): Iterator<number> { yield this.side; }
        method?(): void;
        overload(x: string): string;
        overload(x: number): number;
        overload(x: unknown): unknown { return x; }
    }
    const sq = new Square(3);
    assert(String(sq), "[sq9]");
    assert([...sq][0], 3);
    assert(sq.overload(1), 1);
    assert("extra" in sq, false, "'declare' field emits nothing");
    assert("optional" in sq && "definite" in sq, true, "typed fields are fields");
    assert(typeof sq.method, "undefined");
    assert(Object.keys(sq).join(), "side,kind,optional,definite");

    /* generic classes and expressions */
    const Box = class<T> { constructor(public v: T) {} };
    assert(new Box<number>(2).v, 2);
    class Pair<in out K, const V = unknown> extends Map<K, V> {}
    assert(new Pair<string, number>().size, 0);
}

/* parameter properties: like tsc with useDefineForClassFields, they are
   defined before the other fields and assigned at the start of the
   constructor (after super() in a derived class) */
function test_parameter_properties() {
    const log: string[] = [];
    class Base {
        constructor(public b: number) {
            log.push("base:" + Object.keys(this).join());
        }
    }
    class Derived extends Base {
        field = (log.push("field:" + Object.keys(this).join() + ":" + this.x), 1);
        constructor(public x: number, protected readonly y: string = "dy",
                    private z?: number) {
            log.push("ctor");
            super(x * 2);
            log.push("after:" + Object.keys(this).join() + ":" + this.x);
        }
    }
    const d = new Derived(5);
    assert(log.join("|"), "ctor|base:b|field:b,x,y,z:undefined|after:b,x,y,z,field:5");
    assert(JSON.stringify(d), '{"b":10,"x":5,"y":"dy","field":1}');

    /* base class: fields see the parameter property fields, not values */
    class P {
        a = this.x;
        constructor(public x: number = 1, readonly r = this.a) {}
    }
    const p = new P();
    assert(p.a, undefined);
    assert(p.x, 1);
    assert(Object.keys(p).join(), "x,r,a");

    /* super() in an arrow function */
    class Q extends Base {
        constructor(public q: number) {
            const init = () => super(q);
            init();
        }
    }
    assert(new Q(4).q + new Q(4).b, 8);
}

function test_enums() {
    enum E { A, B, C = 10, D, E = "e", F = D + 1 }
    assert(E.A + E.B + E.C + E.D, 22);
    assert(E[0] + E[11], "AD");
    assert(E.E, "e");
    assert(E["e"], undefined, "no reverse mapping for string members");
    assert(E.F, 12);
    assert(Object.keys(E).join(), "0,1,10,11,12,A,B,C,D,E,F");

    /* computed members and references to earlier members */
    let count = 0;
    function next() { return ++count * 100; }
    enum C { X = next(), Y = X + 1, Z = "abc".length, W = Z * 2 }
    assert(C.X, 100);
    assert(C.Y, 101);
    assert(C[101], "Y");
    assert(C.W, 6);

    /* merged declarations */
    enum M { A = 1 }
    enum M { B = A + 1, C = B * 2 }
    assert(M.A + M.B + M.C, 7);
    assert(M[4], "C");

    /* names: strings and keywords */
    enum N { "a-b" = 1, if = 2, default = N.if + 1 }
    assert(N["a-b"] + N.if + N.default, 6);

    /* constant expressions */
    enum K { A = 1 << 4, B = A | 3, C = ~B, D = -(2 ** 3), E = "x" + "y", F = 7 % 4, G = 100 >>> 2, H = (1 + 2) * 3 }
    assert(K.A + K.B + K.C + K.D, 16 + 19 - 20 - 8);
    assert(K.E + K.F + K.G + K.H, "xy3259");

    /* enum and namespace merge */
    enum Color { Red, Green }
    namespace Color { export function name(c: Color) { return Color[c]; } }
    assert(Color.name(Color.Green), "Green");

    /* block scoped */
    {
        enum Inner { A = 5 }
        assert(Inner.A, 5);
    }
    assert(eval("typeof Inner"), "undefined", "block scoped enum");
}

/* const enums: members are compile time constants */
function test_const_enums() {
    /* used before its declaration: resolved when the variables are */
    function early() { return Later.A; }

    const enum CE { A = 1, B = A * 10, S = "str", T = `t` }
    const enum Later { A = 5 }
    function useCE() { return CE.B + CE["A"]; }
    function useStr() { return CE.S + CE.T; }

    assert(useCE(), 11);
    assert(useStr(), "strt");
    assert(early(), 5);

    /* the enum object is still emitted (tsc's preserveConstEnums) */
    assert(JSON.stringify(CE), '{"1":"A","10":"B","A":1,"B":10,"S":"str","T":"t"}');

    /* folded: changing the object has no effect on the compiled code */
    let reads = 0;
    Object.defineProperty(CE, "B", { get() { reads++; return 99; } });
    Object.defineProperty(Later, "A", { get() { reads++; return 99; } });
    assert(useCE(), 11);
    assert(early(), 5);
    assert(reads, 0);
    assert(useCE.toString().includes("CE.B"), true, "the source is kept");

    /* dynamic accesses use the object */
    const key = "B";
    assert(CE[key], 99);
    assert(reads, 1);

    /* constant expressions only */
    assertSyntaxError("let x = 1; const enum Bad { A = x }");
    assertSyntaxError("const enum Bad { A = Math.PI }");

    /* references to other enums */
    const enum Other { X = CE.A + Later.A, Y = Other.X * 2 }
    assert(Other.Y, 12);

    /* shadowing bindings are not folded */
    function shadowParam(CE: { B: number }) { return CE.B; }
    assert(shadowParam({ B: 3 }), 3);
    function shadowVar() { var CE = { B: 4 }; return CE.B; }
    assert(shadowVar(), 4);
    {
        let CE = { A: 6 };
        assert(CE.A, 6);
    }

    /* in a namespace */
    namespace NS { export const enum K { X = 7 } export const viaMember = K.X; }
    function useNS() { return NS.K.X; }
    Object.defineProperty(NS.K, "X", { get() { return 0; } });
    assert(useNS() + NS.viaMember, 14);
}

function test_namespaces() {
    namespace A.B.C { export const v = 1; }
    assert(A.B.C.v, 1);

    /* exported variables are properties of the namespace object, also
       for the code of the namespace */
    namespace Counter {
        export let count = 0;
        export function inc() { return ++count; }
        export function get() { return count; }
        let hidden = 5;
        export const [first, second] = [hidden, hidden + 1];
        export const { prop } = { prop: "p" };
    }
    Counter.inc();
    Counter.count += 10;
    assert(Counter.get(), 11);
    assert(Counter.first + Counter.second, 11);
    assert(Counter.prop, "p");
    assert(Object.keys(Counter).includes("hidden"), false);

    /* merged blocks see the exports of the other blocks */
    namespace Merged { export const x = 1; }
    namespace Merged { export const y = x + 1; export function f() { return z; } }
    namespace Merged { export const z = y + 1; }
    assert(Merged.f(), 3);

    /* merging with functions and classes */
    function Fn() { return 1; }
    namespace Fn { export const extra = 2; }
    assert(Fn() + Fn.extra, 3);
    class Cls { static s = 1; }
    namespace Cls { export const t = 2; }
    assert(Cls.s + Cls.t, 3);

    /* nested namespaces, classes, enums and non-exported values */
    namespace Outer {
        const secret = 10;
        export namespace Inner { export const w = secret + 1; }
        export class K { v = Inner.w; }
        export enum E { A = Inner.w }
        export import Alias = Inner.w;
    }
    assert(Outer.Inner.w + new Outer.K().v + Outer.E.A + Outer.Alias, 44);
    assert(Outer.E[11], "A");

    /* type-only namespaces emit nothing */
    namespace Types { export interface I {} export type T = number; namespace Deep { export type U = string; } }
    assert(typeof Types, "undefined");

    /* 'export declare' members exist at runtime (assigned from outside) */
    namespace Ambient { export declare let late: number; export function read() { return late; } }
    (Ambient as unknown as { late: number }).late = 8;
    assert(Ambient.read(), 8);

    /* import aliases */
    import Alias = Outer.Inner;
    import TypeAlias = Types.T;
    assert(Alias.w, 11);

    /* 'this' and 'return' */
    assertSyntaxError("namespace N { return; }");
}

function test_rejected() {
    /* TypeScript syntax is not JavaScript */
    assertSyntaxError("let x: number = 1;", false);
    assertSyntaxError("enum E { A }", false);
    assertSyntaxError("function f<T>(x: T) {}", false);
    assertSyntaxError("type T = number;", false);
    /* unsupported constructs */
    /* parameter decorators need experimentalDecorators */
    assertSyntaxError("class C { m(@dec x) {} }");
    assertSyntaxError("@dec function f() {}");
    assertSyntaxError("import x = require('x');");
    assertSyntaxError("const enum E { A = 1, B = Math.random() }");
    assertSyntaxError("enum E { A = Math.random(), B }");
    assertSyntaxError("declare function f() {}");
    /* TypeScript errors */
    assertSyntaxError("let x: = 1;");
    assertSyntaxError("function f(x: number: string) {}");
    /* direct eval() and Function() are JavaScript */
    assertThrows(SyntaxError, () => evalTS("eval('let x: number = 1')"));
    assertThrows(SyntaxError, () => Function("let x: number = 1"));
}

/* code valid in both languages has the same meaning */
function test_js_compatibility() {
    const src = `(function () {
        var log = [];
        var type = 1, declare = 2, abstract = 3;
        var a = 1, b = 2, c = 3, x = 5, y = 6, z = 7;
        log.push(a < b, a < b > c, x ? (y) : z, typeof type);
        var t = true ? (y) : (z) => z;
        log.push(typeof t);
        var f = (p, q = 2) => p + q;
        log.push(f(1));
        class K { static s = 1; get g() { return 2; } readonly = 3; declare() { return 4; } }
        log.push(K.s + new K().g + new K().readonly + new K().declare());
        label: { break label; }
        var re = /[a-z]+/g;
        log.push("abc".match(re)[0], \`t\${a}\`);
        var o = { async: 1, get: 2, set: 3, static: 4, type: 5 };
        log.push(o.async + o.get + o.set + o.static + o.type);
        function* g() { yield 1; }
        log.push([...g()].length);
        var n = a
        !b;
        log.push(n);
        return log.join();
    })()`;
    assert(evalTS(src, true), evalTS(src, false));
}

/* TypeScript 5.2 'using' and 'await using' declarations */
async function test_using() {
    const log: string[] = [];
    function res(name: string): Disposable {
        return { [Symbol.dispose]() { log.push(name); } };
    }
    function f(): number {
        using a: Disposable = res("a"), b: Disposable | null = null;
        using c = res("c") as Disposable;
        for (using x of [res("x")] as Disposable[])
            log.push("loop");
        return log.push("body");
    }
    assert(f(), 3);
    assert(log.join(), "loop,x,body,c,a");
    log.length = 0;
    await (async (): Promise<void> => {
        await using d: AsyncDisposable = {
            async [Symbol.asyncDispose](): Promise<void> { log.push("d"); },
        };
        using s = new DisposableStack();
        s.defer((): void => { log.push("s"); });
    })();
    assert(log.join(), "s,d");
    /* 'using' is still an identifier */
    assert(evalTS("let using: number = 1; using + 1"), 2);
    assertSyntaxError("using x: Disposable = null;");
    assertSyntaxError("function f() { using x: Disposable; }");
    assertSyntaxError("function f() { using { x }: Disposable = null; }");
    assertSyntaxError("declare using x: Disposable;");
}

async function test_modules() {
    /* import elision across files: see tests/ts_modules/main.ts. A dynamic
       import so that the module loading is part of the test. */
    const m = await import("./ts_modules/main.js");
    const r = m.results();
    /* types.ts and widget.ts are only used as types: not loaded */
    assert(r.log, "lib,mixed,side");
    assert(r.norm, "7:5");
    assert(r.flags, 3);
    assert(r.dir, "2DownL");
    assert(r.dist, 5);
    assert(r.origin, true);
    assert(r.thing, 5);
    assert(r.used, 43);
    assert(r.ext, "ext/mts");
    assert(r.w, true);
}

test_type_erasure();
test_type_grammar();
test_expressions();
test_asi_contextual_keywords();
test_classes();
test_parameter_properties();
test_enums();
test_const_enums();
test_namespaces();
test_rejected();
test_js_compatibility();
await test_using();
await test_modules();
