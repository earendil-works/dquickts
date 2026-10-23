import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { describe, it } from "node:test";
import {
	EvalFlags,
	Intrinsics,
	JSException,
	type JSValueHandle,
	MAX_STACK_SIZE,
	PromiseState,
	QuickJS,
	type QuickJSOptions,
} from "../src/index.ts";

const wasm = await WebAssembly.compile(readFileSync(new URL("../quickjs.wasm", import.meta.url)));

async function create(options: Partial<QuickJSOptions> = {}): Promise<QuickJS> {
	return QuickJS.create({ wasm, maxStackSize: 256 * 1024, ...options });
}

function thrown(fn: () => unknown): JSException {
	try {
		fn();
	} catch (error) {
		assert.ok(error instanceof JSException, `expected a JSException, got ${error}`);
		return error;
	}
	assert.fail("expected an exception");
}

describe("evaluation", () => {
	it("evaluates JavaScript and TypeScript", async () => {
		using vm = await create();
		assert.equal(vm.evalCode("1 + 2").toNumber(), 3);
		const ts = "enum Color { Red = 1, Green }\nconst c: Color = Color.Green;\nc satisfies number";
		assert.equal(vm.evalCode(ts, "a.ts", EvalFlags.TYPESCRIPT).toNumber(), 2);
		assert.equal(vm.versions.quickjs, readFileSync(new URL("../../VERSION", import.meta.url), "utf8").trim());
	});

	it("converts values", async () => {
		using vm = await create();
		const value = vm.evalCode("({ a: [1, 'two', null], b: true, c: undefined })");
		assert.deepEqual(value.dump(), { a: [1, "two", null], b: true });
		assert.equal(value.getProp("a").getProp(1).toString(), "two");
		assert.equal(value.getProp("b").toBoolean(), true);
		assert.equal(value.getProp("c").isUndefined, true);
		assert.equal(value.getProp("a").isArray, true);
		assert.equal(vm.evalCode("(() => 1)").typeof, "function");
		assert.equal(vm.evalCode("10n").typeof, "bigint");
		assert.equal(vm.null.isNull, true);
		assert.equal(vm.null.isObject, false);

		const copy = vm.newValue({ x: [1, { y: "z" }] });
		vm.global.setProp("copy", copy);
		assert.equal(vm.evalCode("copy.x[1].y").toString(), "z");
		const obj = vm.newObject();
		obj.setProp("n", vm.newNumber(4));
		obj.setProp(0, vm.newString("zero"));
		assert.equal(obj.toJSON(), '{"0":"zero","n":4}');
	});

	it("keeps strings intact, including lone surrogates", async () => {
		using vm = await create();
		const text = "a\u00e9\u{1f600}\ud800b\udc00";
		using identity = vm.evalCode("(s) => s + ''");
		assert.equal(vm.callFunction(identity, vm.undefined, vm.newString(text)).toString(), text);
		assert.equal(vm.evalCode("'\\ud800' + 'x'").toString(), "\ud800x");
		const long = "x\u00e9".repeat(100_000);
		assert.equal(vm.newString(long).toString(), long);
	});

	it("throws exceptions as JSException with the VM's name, message and stack", async () => {
		using vm = await create();
		const error = thrown(() => vm.evalCode("function f() { throw new TypeError('boom') }\nf()", "script.js"));
		assert.equal(error.name, "TypeError");
		assert.equal(error.message, "boom");
		assert.match(error.stack ?? "", /at f \(script\.js:1/);
		assert.equal(error.handle.isError, true);
		assert.equal(error.uncatchable, false);
		error.dispose();

		const primitive = thrown(() => vm.evalCode("throw 'plain'"));
		assert.equal(primitive.message, "plain");
		assert.equal(thrown(() => vm.evalCode("throw null")).message, "null");
		assert.equal(thrown(() => vm.evalCode("let x = ;")).name, "SyntaxError");
	});

	it("turns deep recursion into a catchable error", async () => {
		using vm = await create();
		const result = vm.evalCode("let depth = 0; function f() { depth++; f() } try { f() } catch (e) { [e.name, depth] }");
		const [name, depth] = result.dump() as [string, number];
		assert.equal(name, "InternalError");
		assert.ok(depth > 500, `depth ${depth}`);
	});

	it("validates the stack size", async () => {
		await assert.rejects(create({ maxStackSize: MAX_STACK_SIZE + 1 }), RangeError);
	});
});

describe("host functions", () => {
	it("calls host functions with borrowed arguments", async () => {
		using vm = await create();
		const seen: unknown[] = [];
		const fn = vm.newFunction("record", function (this: JSValueHandle, ...args) {
			seen.push(this.isUndefined, args.length, ...args.map((arg) => arg.dump()));
			return vm.newNumber(args.length);
		});
		vm.global.setProp("record", fn);
		assert.equal(vm.evalCode("record(1, 'a', { b: 2 })").toNumber(), 3);
		assert.deepEqual(seen, [true, 3, 1, "a", { b: 2 }]);
		assert.equal(vm.evalCode("record.name").toString(), "record");
	});

	it("keeps arguments with dup()", async () => {
		using vm = await create();
		let kept: JSValueHandle | undefined;
		vm.global.setProp(
			"keep",
			vm.newFunction("keep", (value) => {
				kept = value.dup();
			}),
		);
		vm.evalCode("keep({ v: 42 })");
		vm.runGC();
		assert.equal(kept?.getProp("v").toNumber(), 42);
	});

	it("throws host errors into the VM", async () => {
		using vm = await create();
		vm.global.setProp(
			"fail",
			vm.newFunction("fail", () => {
				throw new RangeError("from host");
			}),
		);
		assert.deepEqual(vm.evalCode("try { fail() } catch (e) { [e instanceof Error, e.name, e.message] }").dump(), [
			true,
			"RangeError",
			"from host",
		]);
		// a JSException thrown by a host function keeps its value
		vm.global.setProp(
			"rethrow",
			vm.newFunction("rethrow", (fn) => vm.callFunction(fn, vm.undefined)),
		);
		assert.equal(vm.evalCode("try { rethrow(() => { throw { code: 7 } }) } catch (e) { e.code }").toNumber(), 7);
	});

	it("keeps an interruption uncatchable through host functions", async () => {
		let interrupt = false;
		using vm = await create({ interruptHandler: () => interrupt });
		vm.global.setProp(
			"call",
			vm.newFunction("call", (fn) => vm.callFunction(fn, vm.undefined)),
		);
		vm.global.setProp(
			"interrupt",
			vm.newFunction("interrupt", () => {
				interrupt = true;
			}),
		);
		const interrupted = thrown(() =>
			vm.evalCode("try { call(() => { interrupt(); for (;;); }) } catch { 'caught' }"),
		);
		assert.equal(interrupted.uncatchable, true);
		assert.equal(interrupted.message, "interrupted");
	});
});

describe("promises and jobs", () => {
	it("runs promise jobs one at a time", async () => {
		using vm = await create();
		const promise = vm.evalCode("(async () => { await null; await null; return 5 })()");
		assert.equal(promise.promiseState, PromiseState.PENDING);
		assert.equal(vm.isJobPending(), true);
		assert.equal(vm.executePendingJob(), true);
		assert.ok(vm.executePendingJobs() >= 1);
		assert.equal(vm.isJobPending(), false);
		assert.equal(vm.executePendingJob(), false);
		assert.equal(promise.promiseState, PromiseState.FULFILLED);
		assert.equal(promise.promiseResult.toNumber(), 5);
	});

	it("settles promises from the host", async () => {
		using vm = await create();
		const deferred = vm.newPromise();
		using then = vm.evalCode("(p) => p.then((v) => 'got ' + v)");
		const result = vm.callFunction(then, vm.undefined, deferred.promise);
		deferred.resolve(vm.newString("value"));
		vm.executePendingJobs();
		assert.equal(result.promiseResult.toString(), "got value");
		deferred.dispose();
	});

	it("evaluates top-level await", async () => {
		using vm = await create();
		const promise = vm.evalCode("await Promise.resolve(3)", "<eval>", EvalFlags.ASYNC);
		vm.executePendingJobs();
		assert.equal(promise.promiseResult.getProp("value").toNumber(), 3);
	});

	it("rejects the promise of an async function that is interrupted", async () => {
		// The engine turns an exception in a job into a rejection, an interruption included: a script cannot catch it
		// with try/catch, but sees it as a rejection. Hosts should stop running jobs once they interrupt.
		let interrupt = false;
		using vm = await create({ interruptHandler: () => interrupt });
		const promise = vm.evalCode("(async () => { try { await null; for (;;); } catch { return 'caught' } })()");
		interrupt = true;
		vm.executePendingJobs();
		assert.equal(promise.promiseState, PromiseState.REJECTED);
		assert.equal(promise.promiseResult.toString(), "InternalError: interrupted");
	});
});

describe("limits", () => {
	it("throws a catchable out of memory error", async () => {
		using vm = await create({ memoryLimit: 4 * 1024 * 1024 });
		const result = vm.evalCode("try { const a = []; for (;;) a.push({}) } catch (e) { [e.name, e.message] }");
		assert.deepEqual(result.dump(), ["InternalError", "out of memory"]);
		assert.ok(vm.memoryUsed <= 4 * 1024 * 1024);
		// the memory is free again after the script
		vm.runGC();
		assert.equal(vm.evalCode("[1, 2, 3].length").toNumber(), 3);
	});

	it("interrupts a running script", async () => {
		let polls = 0;
		using vm = await create({ interruptHandler: () => ++polls > 100 });
		const error = thrown(() => vm.evalCode("try { for (;;); } catch { 'caught' }"));
		assert.equal(error.uncatchable, true);
		assert.equal(error.name, "InternalError");
	});
});

describe("environment", () => {
	it("selects intrinsics", async () => {
		using vm = await create({ intrinsics: Intrinsics.ALL & ~Intrinsics.WEAK_REF });
		assert.equal(vm.evalCode("typeof WeakRef + typeof FinalizationRegistry + typeof Map").toString(), "undefinedundefinedfunction");
		using full = await create();
		assert.equal(full.evalCode("typeof WeakRef + typeof performance + typeof atob").toString(), "functionobjectfunction");
		await assert.rejects(create({ intrinsics: Intrinsics.ALL | Intrinsics.TEMPORAL }), /Temporal/);
	});

	it("uses the given time zone", async () => {
		using fixed = await create({ timezoneOffset: -330 });
		assert.equal(fixed.evalCode("new Date(0).getTimezoneOffset() + ' ' + new Date(0).getHours()").toString(), "-330 5");
		using utc = await create({ timezoneOffset: 0 });
		assert.equal(utc.evalCode("new Date(2020, 6, 1).toISOString()").toString(), "2020-07-01T00:00:00.000Z");
	});

	it("takes clocks and randomness from the WASI overrides", async () => {
		const options: Partial<QuickJSOptions> = {
			wasi: (memory) => ({
				clock_time_get(_id: number, _precision: bigint, resultPtr: number) {
					new DataView(memory.buffer).setBigUint64(resultPtr, 1_700_000_000_000n * 1_000_000n, true);
					return 0;
				},
			}),
		};
		using a = await create(options);
		using b = await create(options);
		assert.equal(a.evalCode("Date.now()").toNumber(), 1_700_000_000_000);
		const randoms = "[Math.random(), Math.random()].join()";
		assert.equal(a.evalCode(randoms).toString(), b.evalCode(randoms).toString());
	});

	it("discards output with an fd_write override", async () => {
		let written = 0;
		using vm = await create({
			wasi: (memory) => ({
				fd_write(_fd: number, iovs: number, count: number, nwritten: number) {
					const view = new DataView(memory.buffer);
					let total = 0;
					for (let i = 0; i < count; i++) total += view.getUint32(iovs + i * 8 + 4, true);
					written += total;
					view.setUint32(nwritten, total, true);
					return 0;
				},
			}),
		});
		assert.equal(vm.evalCode("1").toNumber(), 1);
		assert.equal(written, 0);
	});
});

describe("handles", () => {
	it("disposes the handles of a scope", async () => {
		using vm = await create();
		const before = vm.memoryUsed;
		let escaped: JSValueHandle | undefined;
		const inner = vm.withScope((scope) => {
			const big = vm.evalCode("'x'.repeat(1 << 20)");
			escaped = scope.escape(vm.evalCode("({ kept: true })"));
			return big;
		});
		assert.equal(inner.disposed, true);
		assert.equal(escaped?.disposed, false);
		vm.runGC();
		assert.ok(vm.memoryUsed < before + (1 << 19));
	});

	it("releases everything with the VM", async () => {
		const vm = await create();
		const kept = vm.evalCode("({})");
		vm.newFunction("f", () => undefined);
		vm.dispose();
		assert.equal(vm.disposed, true);
		assert.equal(kept.disposed, true);
		assert.throws(() => vm.evalCode("1"), /disposed/);
	});
});
