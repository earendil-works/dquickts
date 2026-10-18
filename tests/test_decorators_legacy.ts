// TypeScript legacy decorators (experimentalDecorators) and decorator
// metadata (emitDecoratorMetadata): run with --emit-decorator-metadata
import { assert, assertThrows } from "./assert.js";

declare const std: {
    evalScript(src: string, options: Record<string, boolean>): unknown;
};

type Decorator = (...args: unknown[]) => unknown;
type Ctor = new (...args: unknown[]) => object;

/* minimal reflect-metadata: the metadata is stored on the target */
interface ReflectMetadata {
    metadata?(k: string, v: unknown): Decorator;
    getMetadata(k: string, target: object, key?: string | symbol): unknown;
    decorate?(decs: Decorator[], target: object, key?: string | symbol,
              desc?: PropertyDescriptor): unknown;
}
/* the global Reflect object is extended like reflect-metadata does */
const R = Reflect as unknown as ReflectMetadata;
const kMetadata = Symbol("metadata");
type MetadataHolder = { [kMetadata]?: Map<unknown, Map<string, unknown>> };

function defineMetadata(k: string, v: unknown): Decorator {
    return (target, key) => {
        const holder = target as MetadataHolder;
        if (!Object.hasOwn(holder, kMetadata))
            holder[kMetadata] = new Map();
        let m = holder[kMetadata]!.get(key);
        if (!m)
            holder[kMetadata]!.set(key, m = new Map());
        m.set(k, v);
    };
}
R.metadata = defineMetadata;
R.getMetadata = (k, target, key) => {
    const holder = target as MetadataHolder;
    return Object.hasOwn(holder, kMetadata) ?
        holder[kMetadata]!.get(key)?.get(k) : undefined;
};

function typeName(t: unknown): string {
    return typeof t === "function" ? t.name : String(t);
}

function typeNames(t: unknown): string {
    return Array.isArray(t) ? t.map(typeName).join() : String(t);
}

function test_order() {
    const log: string[] = [];
    const dec = (name: string): Decorator => {
        log.push("eval " + name);
        return (...args) => {
            const index = args[2];
            log.push(name + (typeof index === "number" ? " " + index : ""));
        };
    };
    log.push("start");
    @dec("c1") @dec("c2")
    class C {
        @dec("sf") static sf = log.push("sf value");
        @dec("m1") @dec("m2") m(@dec("p0") a: number, @dec("p1") b: number) {}
        @dec("sm") static sm() {}
        @dec("f") f = 1;
        constructor(@dec("cp0") x: number) {}
        static { log.push("static block"); }
    }
    assert(log.join(),
           "start,sf value,static block," +
           /* instance members, static members, class */
           "eval m1,eval m2,eval p0,eval p1,p1 1,p0 0,m2,m1," +
           "eval f,f,eval sf,sf,eval sm,sm," +
           "eval c1,eval c2,eval cp0,cp0 0,c2,c1");
}

function test_descriptors() {
    const log: string[] = [];
    const readonly = (target: object, key: string, desc: PropertyDescriptor) => {
        desc.writable = false;
        return desc;
    };
    const wrap = (target: object, key: string,
                  desc: PropertyDescriptor): PropertyDescriptor => {
        const m = desc.value as (...args: unknown[]) => unknown;
        return {
            ...desc,
            value(this: unknown, ...args: unknown[]) { return "<" + m.apply(this, args) + ">"; },
        };
    };
    const upper = (target: object, key: string,
                   desc: PropertyDescriptor): PropertyDescriptor => {
        const get = desc.get!;
        return { ...desc, get() { return String(get.call(this)).toUpperCase(); } };
    };
    const onProto = (target: object, key: string): PropertyDescriptor => {
        log.push(key + ":" + (target === Svc.prototype) + ":" + (target === Svc));
        return { value: "from descriptor", writable: true, configurable: true };
    };
    class Svc {
        @readonly ro() { return 1; }
        @wrap greet(n: string) { return "hi " + n; }
        @upper get name() { return "svc"; }
        @wrap static st() { return "st"; }
        @onProto p: string;
        @onProto static sp: string;
    }
    const s = new Svc();
    assert(s.greet("x"), "<hi x>");
    assert(s.name, "SVC");
    assert(Svc.st(), "<st>");
    assert(Object.getOwnPropertyDescriptor(Svc.prototype, "ro")!.writable, false);
    assert(log.join(), "p:true:false,sp:false:true");
    assert(Object.getOwnPropertyDescriptor(Svc.prototype, "p")!.value,
           "from descriptor");
    /* class decorators replace the class, the class name refers to the
       decorated class */
    const sealed = <T extends Ctor>(cls: T) => class extends cls { sealed = true; };
    @sealed
    class Box {
        static make() { return new Box(); }
    }
    assert("sealed" in Box.make(), true);
    assert("sealed" in new Box(), true);
    /* computed keys and Reflect.decorate() */
    const key = "dyn";
    const calls: string[] = [];
    class K {
        @((t: object, k: string) => { calls.push(k); }) [key]() {}
    }
    R.decorate = (decs, target, k, desc) => {
        calls.push("decorate " + decs.length + " " + String(k));
        return k === undefined ? target : desc;
    };
    @((c: Ctor) => c) class L { @((t: object, k: string) => {}) m() {} }
    delete R.decorate;
    /* with the 3 metadata decorators of the method */
    assert(calls.join(), "dyn,decorate 4 m,decorate 1 undefined");
    /* private members cannot be decorated */
    assertThrows(SyntaxError, () => std.evalScript(
        "class C { @dec #m() {} }",
        { typescript: true, experimentalDecorators: true }));
}

namespace NS {
    export class Repo {}
}
enum Num { A, B }
enum Str { A = "a" }

function test_metadata() {
    const d: Decorator = () => {};
    class Dep {}
    @d
    class Svc {
        @d a: string;
        @d b: number[];
        @d c: Dep | null;
        @d e: NS.Repo;
        @d f: Num;
        @d g: Str;
        @d h: "x" | "y";
        @d i: Promise<void>;
        @d j: () => void;
        @d k;
        @d l: UnknownType;
        constructor(private dep: Dep, repo: NS.Repo, n?: number, ...rest: string[]) {}
        @d m(x: string, { y }: { y: number }): boolean { return true; }
        @d async n() {}
        @d get o(): Dep { return new Dep(); }
        @d set p(v: number) {}
    }
    const t = (k: string) => typeName(R.getMetadata("design:type", Svc.prototype, k));
    assert(["a", "b", "c", "e", "f", "g", "h", "i", "j", "k", "l"].map(t).join(),
           "String,Array,Object,Repo,Number,String,String,Promise,Function,Object,Object");
    assert(typeNames(R.getMetadata("design:paramtypes", Svc)), "Dep,Repo,Number,String");
    assert(t("m"), "Function");
    assert(typeNames(R.getMetadata("design:paramtypes", Svc.prototype, "m")),
           "String,Object");
    assert(typeName(R.getMetadata("design:returntype", Svc.prototype, "m")), "Boolean");
    assert(typeName(R.getMetadata("design:returntype", Svc.prototype, "n")), "Promise");
    assert(t("o") + "," + t("p"), "Dep,Number");
    assert(typeNames(R.getMetadata("design:paramtypes", Svc.prototype, "p")), "Number");
    /* no class decorator: no class metadata */
    class NoDec { constructor(x: string) {} @d m() {} }
    assert(R.getMetadata("design:paramtypes", NoDec), undefined);
    assert(Object.hasOwn(NoDec.prototype, kMetadata), true);
    assert(typeName(R.getMetadata("design:returntype", NoDec.prototype, "m")),
           "undefined");
    /* without emitDecoratorMetadata */
    const r = std.evalScript(
        "const log = []; const d = (t, k) => { log.push(k); };" +
        "class C { @d x: string; @d m(a: number) {} } log.join()",
        { typescript: true, experimentalDecorators: true, emitDecoratorMetadata: false });
    assert(r, "x,m");
    /* Reflect.metadata() is only used if it exists */
    delete R.metadata;
    class Plain { @d x: string; }
    R.metadata = defineMetadata;
    assert(Object.hasOwn(Plain.prototype, kMetadata), false);
}

/* constructor injection with the design:paramtypes metadata */
function test_injection() {
    const instances = new Map<Ctor, object>();
    const injectable = new Set<Ctor>();
    const kTokens = Symbol("tokens");
    type TokenHolder = { [kTokens]?: Map<number, string> };
    const Injectable = (): Decorator => (target) => { injectable.add(target as Ctor); };
    const Inject = (token: string): Decorator => (target, key, index) => {
        const holder = target as TokenHolder;
        (holder[kTokens] ??= new Map()).set(index as number, token);
    };
    const values: Record<string, unknown> = { url: "db://" };
    function resolve<T extends object>(cls: new (...args: never[]) => T): T {
        const ctor = cls as unknown as Ctor; /* erase the parameter types */
        let inst = instances.get(ctor);
        if (!inst) {
            assert(injectable.has(ctor), true);
            const types = R.getMetadata("design:paramtypes", ctor);
            const tokens = (ctor as TokenHolder)[kTokens];
            const args = (Array.isArray(types) ? types : []).map((t, i) => {
                const tok = tokens?.get(i);
                return tok ? values[tok] : resolve(t as Ctor);
            });
            inst = new ctor(...args);
            instances.set(ctor, inst);
        }
        return inst as T;
    }
    @Injectable()
    class Db {
        constructor(@Inject("url") readonly url: string) {}
    }
    @Injectable()
    class Users {
        constructor(readonly db: Db) {}
        find() { return this.db.url + "users"; }
    }
    @Injectable()
    class App {
        constructor(private users: Users, private db: Db) {}
        run() { return this.users.find() + ":" + (this.users.db === this.db); }
    }
    assert(resolve(App).run(), "db://users:true");
}

test_order();
test_descriptors();
test_metadata();
test_injection();
