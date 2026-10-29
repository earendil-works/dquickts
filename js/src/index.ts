/**
 * JavaScript bindings for dquickts compiled to WebAssembly.
 *
 * Each `QuickJS` VM is one instance of quickjs.wasm holding one runtime and one context. Values inside the VM are
 * reached through `JSValueHandle`s, which must be disposed (directly, with `using`, or with `withScope()`).
 *
 * ```ts
 * const vm = await QuickJS.create({ wasm: await WebAssembly.compile(bytes) });
 * const result = vm.evalCode("1 + 2");
 * console.log(result.toNumber()); // 3
 * result.dispose();
 * vm.dispose();
 * ```
 */
import { createWasi, type WasiFunction, type WasiOverrides, type WasmMemory } from "./wasi.ts";

export type { WasiFunction, WasiOverrides, WasmMemory } from "./wasi.ts";

/** Version of these bindings. */
export const VERSION = "0.1.0";

/**
 * Largest `maxStackSize`, and its default. quickjs.wasm has 1 MiB of stack; the rest is for the C frames below the
 * deepest JS frame.
 */
export const MAX_STACK_SIZE = 512 * 1024;

/** Flags for `evalCode()` (the `JS_EVAL_*` flags of quickjs.h). */
export const EvalFlags = {
	/** Global code (the default). */
	GLOBAL: 0,
	/** Module code. The result is the module's evaluation promise. */
	MODULE: 1 << 0,
	/** Force strict mode. */
	STRICT: 1 << 3,
	/** Allow top-level `await` in global code. The result is a promise of an object `{ value }`. */
	ASYNC: 1 << 7,
	/** TypeScript: types are erased while parsing. */
	TYPESCRIPT: 1 << 8,
	/** TypeScript legacy (experimental) decorators instead of standard decorators. */
	TS_LEGACY_DECORATORS: 1 << 9,
	/** Emit `design:*` decorator metadata (with `TS_LEGACY_DECORATORS`). */
	TS_DECORATOR_METADATA: 1 << 10,
} as const;

/** Builtins of the context, for the `intrinsics` option. The base objects are always present. */
export const Intrinsics = {
	DATE: 1 << 0,
	EVAL: 1 << 1,
	REGEXP: 1 << 2,
	JSON: 1 << 3,
	PROXY: 1 << 4,
	MAP_SET: 1 << 5,
	TYPED_ARRAYS: 1 << 6,
	PROMISE: 1 << 7,
	/** Always present; accepted for compatibility. */
	BIG_INT: 1 << 8,
	/** `WeakRef` and `FinalizationRegistry`. */
	WEAK_REF: 1 << 9,
	PERFORMANCE: 1 << 10,
	DOM_EXCEPTION: 1 << 11,
	ATOB_BTOA: 1 << 12,
	STRING_NORMALIZE: 1 << 13,
	/** Only in a quickjs.wasm built with `CONFIG_TEMPORAL_INTL`; not part of `ALL`. */
	TEMPORAL: 1 << 14,
	/** Only in a quickjs.wasm built with `CONFIG_TEMPORAL_INTL`; not part of `ALL`. */
	INTL: 1 << 15,
	/** Everything `JS_NewContext()` adds. */
	ALL: (1 << 14) - 1,
} as const;

/** `JSValueHandle.promiseState` */
export const PromiseState = {
	PENDING: 0,
	FULFILLED: 1,
	REJECTED: 2,
} as const;

export type HostFunction = (this: JSValueHandle, ...args: JSValueHandle[]) => JSValueHandle | undefined | void;

export interface QuickJSOptions {
	/** quickjs.wasm, compiled or as bytes. */
	wasm: WebAssembly.Module | BufferSource;
	/** Bytes the engine may allocate. Allocations beyond it throw an "out of memory" InternalError. */
	memoryLimit?: number;
	/** Guard on the JS stack, which turns deep recursion into a catchable error. Default and maximum: `MAX_STACK_SIZE`. */
	maxStackSize?: number;
	/**
	 * Polled while JS runs. Returning true interrupts the execution with an uncatchable error. A handler that throws
	 * interrupts too. No JS code runs after that until the call into the VM (`evalCode()`, `callFunction()`,
	 * `executePendingJob()`...) returns: it throws the interruption, also if an async function turned it into a
	 * rejection.
	 */
	interruptHandler?: () => boolean;
	/** Replaces the default WASI functions (clocks, random bytes, output). */
	wasi?: WasiOverrides;
	/**
	 * Time zone of `Date`: `"host"` (the default) uses the host's, a number is a fixed offset in minutes and a function
	 * returns the offset for a time in seconds since the epoch. Offsets follow `Date.prototype.getTimezoneOffset()`:
	 * minutes, positive west of UTC (UTC-8 is 480).
	 */
	timezoneOffset?: "host" | number | ((seconds: number) => number);
	/** Builtins of the context (`Intrinsics` flags). Default: `Intrinsics.ALL`. */
	intrinsics?: number;
}

/** A batch of handles created in `withScope()`. */
export interface HandleScope {
	/** Keeps `handle` alive past the scope (it moves to the enclosing scope, if any). */
	escape<T extends JSValueHandle>(handle: T): T;
}

/** A promise created by `newPromise()`, with its resolving functions. */
export interface Deferred {
	promise: JSValueHandle;
	resolve(value?: JSValueHandle): void;
	reject(reason?: JSValueHandle): void;
	dispose(): void;
}

interface Exports {
	memory: WebAssembly.Memory;
	_initialize(): void;
	qjs_version(): number;
	qjs_sizeof_value(): number;
	qjs_has_temporal_intl(): number;
	qjs_init(intrinsics: number, memoryLimit: number, maxStackSize: number): number;
	qjs_destroy(): void;
	qjs_set_memory_limit(limit: number): void;
	qjs_set_max_stack_size(size: number): void;
	qjs_set_interrupt_handler(enable: number): void;
	qjs_set_gc_threshold(threshold: number): void;
	qjs_run_gc(): void;
	qjs_memory_used(): number;
	qjs_malloc(size: number): number;
	qjs_free(ptr: number): void;
	qjs_eval(code: number, len: number, filename: number, flags: number): number;
	qjs_call(func: number, thisVal: number, argc: number, argv: number): number;
	qjs_call_constructor(func: number, argc: number, argv: number): number;
	qjs_new_function(id: number, name: number, length: number): number;
	qjs_is_job_pending(): number;
	qjs_execute_pending_job(): number;
	qjs_get_exception(): number;
	qjs_is_uncatchable_exception(): number;
	qjs_throw(val: number, uncatchable: number): void;
	qjs_new_error(): number;
	qjs_dup_value(val: number): number;
	qjs_free_value(val: number): void;
	qjs_get_undefined(): number;
	qjs_get_null(): number;
	qjs_new_bool(b: number): number;
	qjs_new_number(n: number): number;
	qjs_new_string(str: number, len: number): number;
	qjs_new_object(): number;
	qjs_new_array(): number;
	qjs_get_global(): number;
	qjs_parse_json(json: number, len: number, filename: number): number;
	qjs_json_stringify(val: number): number;
	qjs_typeof(val: number): number;
	qjs_is_null(val: number): number;
	qjs_is_array(val: number): number;
	qjs_is_error(val: number): number;
	qjs_is_promise(val: number): number;
	qjs_to_bool(val: number): number;
	qjs_to_number(val: number): number;
	qjs_has_exception(): number;
	qjs_to_string(val: number): number;
	qjs_last_length(): number;
	qjs_free_cstring(str: number): void;
	qjs_get_prop(obj: number, name: number): number;
	qjs_set_prop(obj: number, name: number, val: number): number;
	qjs_get_prop_value(obj: number, key: number): number;
	qjs_set_prop_value(obj: number, key: number, val: number): number;
	qjs_get_index(obj: number, idx: number): number;
	qjs_set_index(obj: number, idx: number, val: number): number;
	qjs_new_promise(out: number): number;
	qjs_promise_state(promise: number): number;
	qjs_promise_result(promise: number): number;
}

/** Results of `qjs_typeof()`. */
const TYPEOF = ["undefined", "object", "boolean", "number", "string", "symbol", "bigint", "function"] as const;
const TYPE_EXCEPTION = 8;

/** A JS value thrown by code in the VM. */
export class JSException extends Error {
	/** The thrown value. Disposed with the exception or the VM. */
	readonly handle: JSValueHandle;
	/** True for errors scripts cannot catch, such as an interruption. */
	readonly uncatchable: boolean;
	#stack: string | undefined;

	/** @internal */
	constructor(handle: JSValueHandle, uncatchable: boolean) {
		super();
		this.handle = handle;
		this.uncatchable = uncatchable;
		// V8 defines an own `stack` property, which would hide the accessor below.
		delete (this as { stack?: string }).stack;
		const vm = handle.vm;
		let name = "Error";
		let message: string;
		let stack: string | undefined;
		if (handle.typeof === "object" && !handle.isNull) {
			name = vm._readStringProp(handle, "name") ?? "Error";
			message = vm._readStringProp(handle, "message") ?? vm._safeToString(handle);
			stack = vm._readStringProp(handle, "stack");
		} else {
			message = vm._safeToString(handle);
		}
		this.name = name;
		this.message = message;
		this.#stack = stack;
	}

	/** The stack of the VM (frames only, as QuickJS writes them), not of the host. */
	override get stack(): string | undefined {
		return this.#stack;
	}

	override set stack(value: string | undefined) {
		this.#stack = value;
	}

	dispose(): void {
		this.handle.dispose();
	}

	[Symbol.dispose](): void {
		this.dispose();
	}
}

const HANDLE_OWNED = 0;
/** cached by the VM (undefined, null, booleans, global object); `dispose()` does nothing */
const HANDLE_SINGLETON = 1;
/** `this` and arguments of a host function, owned by the caller; valid during the call only */
const HANDLE_BORROWED = 2;

/** A value inside a VM. */
export class JSValueHandle {
	readonly vm: QuickJS;
	/** @internal address of the JSValue in the memory of the VM */
	readonly ptr: number;
	#kind: number;
	#disposed = false;

	/** @internal */
	constructor(vm: QuickJS, ptr: number, kind = HANDLE_OWNED) {
		this.vm = vm;
		this.ptr = ptr;
		this.#kind = kind;
		if (kind === HANDLE_OWNED) vm._track(this);
	}

	/** Whether `dispose()` was called. A disposed handle must not be used. */
	get disposed(): boolean {
		return this.#disposed;
	}

	/** Result of the `typeof` operator. */
	get typeof(): "undefined" | "object" | "boolean" | "number" | "string" | "symbol" | "bigint" | "function" {
		return TYPEOF[this.vm._exports.qjs_typeof(this.ptr)] ?? "undefined";
	}

	get isUndefined(): boolean {
		return this.typeof === "undefined";
	}

	get isNull(): boolean {
		return this.vm._exports.qjs_is_null(this.ptr) !== 0;
	}

	get isBoolean(): boolean {
		return this.typeof === "boolean";
	}

	get isNumber(): boolean {
		return this.typeof === "number";
	}

	get isString(): boolean {
		return this.typeof === "string";
	}

	get isSymbol(): boolean {
		return this.typeof === "symbol";
	}

	get isBigInt(): boolean {
		return this.typeof === "bigint";
	}

	/** True for objects and functions, not for null. */
	get isObject(): boolean {
		const type = this.typeof;
		return type === "function" || (type === "object" && !this.isNull);
	}

	get isFunction(): boolean {
		return this.typeof === "function";
	}

	get isArray(): boolean {
		return this.vm._exports.qjs_is_array(this.ptr) !== 0;
	}

	get isError(): boolean {
		return this.vm._exports.qjs_is_error(this.ptr) !== 0;
	}

	get isPromise(): boolean {
		return this.vm._exports.qjs_is_promise(this.ptr) !== 0;
	}

	/** `PromiseState` of a promise; -1 for other values. */
	get promiseState(): number {
		return this.vm._exports.qjs_promise_state(this.ptr);
	}

	/** The value or reason of a settled promise (undefined while pending). */
	get promiseResult(): JSValueHandle {
		return this.vm._wrap(this.vm._exports.qjs_promise_result(this.ptr));
	}

	/** `String(value)`. Throws a `JSException` if the conversion throws. */
	toString(): string {
		return this.vm._toString(this);
	}

	/** `Number(value)`. Throws a `JSException` if the conversion throws. */
	toNumber(): number {
		const exports = this.vm._exports;
		const result = exports.qjs_to_number(this.ptr);
		if (Number.isNaN(result) && exports.qjs_has_exception()) this.vm._throwPending();
		return result;
	}

	/** `Boolean(value)`. */
	toBoolean(): boolean {
		const result = this.vm._exports.qjs_to_bool(this.ptr);
		if (result < 0) this.vm._throwPending();
		return result !== 0;
	}

	/** `JSON.stringify(value)`, or undefined for values JSON does not represent. */
	toJSON(): string | undefined {
		const result = this.vm._check(this.vm._exports.qjs_json_stringify(this.ptr));
		try {
			return result.isUndefined ? undefined : result.toString();
		} finally {
			result.dispose();
		}
	}

	/** The value converted to a host value through JSON (undefined for values JSON does not represent). */
	dump(): unknown {
		const json = this.toJSON();
		return json === undefined ? undefined : JSON.parse(json);
	}

	/** `value[key]` */
	getProp(key: string | number | JSValueHandle): JSValueHandle {
		const vm = this.vm;
		const exports = vm._exports;
		if (typeof key === "number" && Number.isInteger(key) && key >= 0 && key <= 0xfffffffe) {
			return vm._check(exports.qjs_get_index(this.ptr, key));
		}
		if (typeof key === "object") return vm._check(exports.qjs_get_prop_value(this.ptr, key.ptr));
		const name = vm._writeString(String(key));
		try {
			return vm._check(exports.qjs_get_prop(this.ptr, name.ptr));
		} finally {
			exports.qjs_free(name.ptr);
		}
	}

	/** `value[key] = newValue` */
	setProp(key: string | number | JSValueHandle, value: JSValueHandle): void {
		const vm = this.vm;
		const exports = vm._exports;
		let result: number;
		if (typeof key === "number" && Number.isInteger(key) && key >= 0 && key <= 0xfffffffe) {
			result = exports.qjs_set_index(this.ptr, key, value.ptr);
		} else if (typeof key === "object") {
			result = exports.qjs_set_prop_value(this.ptr, key.ptr, value.ptr);
		} else {
			const name = vm._writeString(String(key));
			try {
				result = exports.qjs_set_prop(this.ptr, name.ptr, value.ptr);
			} finally {
				exports.qjs_free(name.ptr);
			}
		}
		if (result < 0) vm._throwPending();
	}

	/** A new handle to the same value, owned by the caller. */
	dup(): JSValueHandle {
		return this.vm._wrap(this.vm._exports.qjs_dup_value(this.ptr));
	}

	/** Calls `fn` with this handle and disposes it afterwards. */
	consume<T>(fn: (handle: JSValueHandle) => T): T {
		try {
			return fn(this);
		} finally {
			this.dispose();
		}
	}

	/** Releases the value. Does nothing for the VM's cached handles and the arguments of host functions. */
	dispose(): void {
		if (this.#disposed || this.#kind !== HANDLE_OWNED) return;
		this.#disposed = true;
		this.vm._untrack(this);
		if (!this.vm.disposed) this.vm._exports.qjs_free_value(this.ptr);
	}

	[Symbol.dispose](): void {
		this.dispose();
	}
}

const MAX_U32 = 0xffffffff;

function validateSize(name: string, value: number | undefined, max = MAX_U32): number {
	if (value === undefined) return 0;
	if (!Number.isInteger(value) || value <= 0 || value > max) {
		throw new RangeError(`${name} must be an integer between 1 and ${max}`);
	}
	return value;
}

/** WTF-8: UTF-8 that keeps lone surrogates, which the engine reads and writes as 3-byte sequences. */
function encodeWtf8(text: string, encoder: TextEncoder): Uint8Array {
	if ((text as { isWellFormed?: () => boolean }).isWellFormed?.() !== false) return encoder.encode(text);
	const bytes: number[] = [];
	for (let i = 0; i < text.length; i++) {
		let c = text.charCodeAt(i);
		if (c >= 0xd800 && c <= 0xdbff && i + 1 < text.length) {
			const d = text.charCodeAt(i + 1);
			if (d >= 0xdc00 && d <= 0xdfff) {
				c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00);
				i++;
			}
		}
		if (c < 0x80) bytes.push(c);
		else if (c < 0x800) bytes.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f));
		else if (c < 0x10000) bytes.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
		else bytes.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 0x3f), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
	}
	return new Uint8Array(bytes);
}

function decodeWtf8(bytes: Uint8Array): string {
	let text = "";
	let chunk: number[] = [];
	for (let i = 0; i < bytes.length; ) {
		const b = bytes[i];
		let c: number;
		if (b < 0x80) {
			c = b;
			i += 1;
		} else if (b < 0xe0) {
			c = ((b & 0x1f) << 6) | (bytes[i + 1] & 0x3f);
			i += 2;
		} else if (b < 0xf0) {
			c = ((b & 0x0f) << 12) | ((bytes[i + 1] & 0x3f) << 6) | (bytes[i + 2] & 0x3f);
			i += 3;
		} else {
			c = ((b & 0x07) << 18) | ((bytes[i + 1] & 0x3f) << 12) | ((bytes[i + 2] & 0x3f) << 6) | (bytes[i + 3] & 0x3f);
			i += 4;
		}
		if (c >= 0x10000) {
			c -= 0x10000;
			chunk.push(0xd800 + (c >> 10), 0xdc00 + (c & 0x3ff));
		} else {
			chunk.push(c);
		}
		if (chunk.length >= 8192) {
			text += String.fromCharCode(...chunk);
			chunk = [];
		}
	}
	return text + String.fromCharCode(...chunk);
}

/** A JavaScript VM: one instance of quickjs.wasm. */
export class QuickJS {
	/** @internal */
	_exports!: Exports;
	#memory!: WebAssembly.Memory;
	#bytes: Uint8Array = new Uint8Array(0);
	#encoder = new TextEncoder();
	#decoder = new TextDecoder("utf-8", { fatal: true });
	#disposed = false;
	#functions = new Map<number, HostFunction>();
	#nextFunctionId = 1;
	#interruptHandler: (() => boolean) | undefined;
	#timezoneOffset: (seconds: number) => number = (seconds) => -new Date(seconds * 1000).getTimezoneOffset() * 60;
	/** handles to release with the VM */
	#owned = new Set<JSValueHandle>();
	#scope: Set<JSValueHandle> | undefined;
	#singletons = new Map<string, JSValueHandle>();
	#versions: Record<string, string> | undefined;
	#hasTemporalIntl = false;

	private constructor() {}

	/** Creates a VM. */
	static async create(options: QuickJSOptions): Promise<QuickJS> {
		const vm = new QuickJS();
		const { wasm } = options;
		const module = wasm instanceof WebAssembly.Module ? wasm : await WebAssembly.compile(wasm);
		const memory: WasmMemory = {
			get buffer() {
				return vm.#memory.buffer as ArrayBuffer;
			},
		};
		const wasi = { ...createWasi(memory), ...options.wasi?.(memory) };
		const instance = await WebAssembly.instantiate(module, {
			wasi_snapshot_preview1: wasi,
			env: {
				host_call: (id: number, thisPtr: number, argc: number, argv: number) => vm.#hostCall(id, thisPtr, argc, argv),
				host_interrupt: () => vm.#interrupt(),
				host_timezone_offset: (seconds: number) => {
					try {
						return vm.#timezoneOffset(seconds) | 0;
					} catch {
						return 0;
					}
				},
			},
		});
		vm._exports = instance.exports as unknown as Exports;
		vm.#memory = vm._exports.memory;
		vm._exports._initialize();
		if (vm._exports.qjs_sizeof_value() !== 8) throw new Error("quickjs.wasm: unexpected JSValue size");
		vm.#hasTemporalIntl = vm._exports.qjs_has_temporal_intl() !== 0;

		const tz = options.timezoneOffset ?? "host";
		if (typeof tz === "number") {
			const seconds = -tz * 60;
			vm.#timezoneOffset = () => seconds;
		} else if (typeof tz === "function") {
			vm.#timezoneOffset = (seconds) => -tz(seconds) * 60;
		}
		const intrinsics = options.intrinsics ?? Intrinsics.ALL;
		if ((intrinsics & (Intrinsics.TEMPORAL | Intrinsics.INTL)) !== 0 && !vm.#hasTemporalIntl) {
			throw new Error("This quickjs.wasm was built without Temporal and Intl");
		}
		const memoryLimit = validateSize("memoryLimit", options.memoryLimit);
		const maxStackSize = validateSize("maxStackSize", options.maxStackSize, MAX_STACK_SIZE) || MAX_STACK_SIZE;
		if (vm._exports.qjs_init(intrinsics >>> 0, memoryLimit, maxStackSize) !== 0) {
			throw new Error("Failed to create the QuickJS runtime");
		}
		if (options.interruptHandler) {
			vm.#interruptHandler = options.interruptHandler;
			vm._exports.qjs_set_interrupt_handler(1);
		}
		return vm;
	}

	/** `dquickts` is the version of these bindings, `quickjs` the version of the engine. */
	get versions(): Record<string, string> {
		this.#versions ??= {
			dquickts: VERSION,
			quickjs: this.#readCString(this._exports.qjs_version()),
		};
		return this.#versions;
	}

	get disposed(): boolean {
		return this.#disposed;
	}

	get undefined(): JSValueHandle {
		return this.#singleton("undefined", () => this._exports.qjs_get_undefined());
	}

	get null(): JSValueHandle {
		return this.#singleton("null", () => this._exports.qjs_get_null());
	}

	get true(): JSValueHandle {
		return this.#singleton("true", () => this._exports.qjs_new_bool(1));
	}

	get false(): JSValueHandle {
		return this.#singleton("false", () => this._exports.qjs_new_bool(0));
	}

	/** The global object. */
	get global(): JSValueHandle {
		return this.#singleton("global", () => this._exports.qjs_get_global());
	}

	/** Bytes allocated by the engine. */
	get memoryUsed(): number {
		this.#assertAlive();
		return this._exports.qjs_memory_used();
	}

	/** Changes the memory limit (0: unlimited). */
	setMemoryLimit(limit: number): void {
		this.#assertAlive();
		this._exports.qjs_set_memory_limit(limit === 0 ? 0 : validateSize("memoryLimit", limit));
	}

	/** Changes the interrupt handler (undefined: none). */
	setInterruptHandler(handler: (() => boolean) | undefined): void {
		this.#assertAlive();
		this.#interruptHandler = handler;
		this._exports.qjs_set_interrupt_handler(handler ? 1 : 0);
	}

	/** Runs the cycle collector. */
	runGC(): void {
		this.#assertAlive();
		this._exports.qjs_run_gc();
	}

	/**
	 * Evaluates code and returns its completion value. Throws a `JSException` if it throws.
	 *
	 * @param flags `EvalFlags`, e.g. `EvalFlags.TYPESCRIPT`
	 */
	evalCode(code: string, filename = "<eval>", flags = 0): JSValueHandle {
		this.#assertAlive();
		const source = this._writeString(code);
		const file = this._writeString(filename);
		try {
			return this._check(this._exports.qjs_eval(source.ptr, source.len, file.ptr, flags));
		} finally {
			this._exports.qjs_free(source.ptr);
			this._exports.qjs_free(file.ptr);
		}
	}

	/** `func.call(thisVal, ...args)`. Throws a `JSException` if the call throws. */
	callFunction(func: JSValueHandle, thisVal: JSValueHandle, ...args: JSValueHandle[]): JSValueHandle {
		this.#assertAlive();
		const argv = this.#writePointers(args);
		try {
			return this._check(this._exports.qjs_call(func.ptr, thisVal.ptr, args.length, argv));
		} finally {
			this._exports.qjs_free(argv);
		}
	}

	/** `new func(...args)`. Throws a `JSException` if the constructor throws. */
	callConstructor(func: JSValueHandle, ...args: JSValueHandle[]): JSValueHandle {
		this.#assertAlive();
		const argv = this.#writePointers(args);
		try {
			return this._check(this._exports.qjs_call_constructor(func.ptr, args.length, argv));
		} finally {
			this._exports.qjs_free(argv);
		}
	}

	/**
	 * A function that calls `fn` on the host. `this` and the arguments are handles valid during the call only (`dup()`
	 * them to keep them). `fn` returns a handle, which stays owned by the host, or undefined. When `fn` throws, the
	 * function throws an `Error` with the same message in the VM; a `JSException` is thrown back as its value.
	 */
	newFunction(name: string, fn: HostFunction, length = 0): JSValueHandle {
		this.#assertAlive();
		const id = this.#nextFunctionId++;
		this.#functions.set(id, fn);
		const namePtr = this._writeString(name);
		try {
			return this._check(this._exports.qjs_new_function(id, namePtr.ptr, length));
		} finally {
			this._exports.qjs_free(namePtr.ptr);
		}
	}

	newString(value: string): JSValueHandle {
		this.#assertAlive();
		const str = this._writeString(value);
		try {
			return this._check(this._exports.qjs_new_string(str.ptr, str.len));
		} finally {
			this._exports.qjs_free(str.ptr);
		}
	}

	newNumber(value: number): JSValueHandle {
		this.#assertAlive();
		return this._check(this._exports.qjs_new_number(value));
	}

	newBoolean(value: boolean): JSValueHandle {
		return value ? this.true : this.false;
	}

	newObject(): JSValueHandle {
		this.#assertAlive();
		return this._check(this._exports.qjs_new_object());
	}

	newArray(): JSValueHandle {
		this.#assertAlive();
		return this._check(this._exports.qjs_new_array());
	}

	/** An `Error`; from a host `Error`, with its name and message (not its stack). */
	newError(error?: string | Error): JSValueHandle {
		this.#assertAlive();
		const handle = this._check(this._exports.qjs_new_error());
		if (error === undefined) return handle;
		const message = typeof error === "string" ? error : error.message;
		this.newString(message).consume((value) => handle.setProp("message", value));
		if (typeof error !== "string" && error.name && error.name !== "Error") {
			this.newString(error.name).consume((value) => handle.setProp("name", value));
		}
		return handle;
	}

	/** `JSON.parse(json)` in the VM. */
	newJSON(json: string): JSValueHandle {
		this.#assertAlive();
		const str = this._writeString(json);
		const file = this._writeString("<json>");
		try {
			return this._check(this._exports.qjs_parse_json(str.ptr, str.len, file.ptr));
		} finally {
			this._exports.qjs_free(str.ptr);
			this._exports.qjs_free(file.ptr);
		}
	}

	/** A host value copied into the VM through JSON (undefined stays undefined). */
	newValue(value: unknown): JSValueHandle {
		if (value === undefined) return this.undefined;
		const json = JSON.stringify(value);
		return json === undefined ? this.undefined : this.newJSON(json);
	}

	/** A pending promise and its resolving functions. */
	newPromise(): Deferred {
		this.#assertAlive();
		const exports = this._exports;
		const out = exports.qjs_malloc(8);
		if (out === 0) throw new Error("quickjs.wasm: out of memory");
		let promise: JSValueHandle;
		let resolveFn: JSValueHandle;
		let rejectFn: JSValueHandle;
		try {
			promise = this._check(exports.qjs_new_promise(out));
			const view = new DataView(this.#memory.buffer);
			resolveFn = this._wrap(view.getUint32(out, true));
			rejectFn = this._wrap(view.getUint32(out + 4, true));
		} finally {
			exports.qjs_free(out);
		}
		const settle = (fn: JSValueHandle, value: JSValueHandle | undefined) => {
			this.callFunction(fn, this.undefined, value ?? this.undefined).dispose();
		};
		return {
			promise,
			resolve: (value) => settle(resolveFn, value),
			reject: (reason) => settle(rejectFn, reason),
			dispose: () => {
				promise.dispose();
				resolveFn.dispose();
				rejectFn.dispose();
			},
		};
	}

	/** Whether promise jobs are waiting to run. */
	isJobPending(): boolean {
		this.#assertAlive();
		return this._exports.qjs_is_job_pending() !== 0;
	}

	/**
	 * Runs one promise job. Returns false if none was pending. Throws a `JSException` if the job threw or was
	 * interrupted. Exceptions in promise reactions and async functions reject their promise instead.
	 */
	executePendingJob(): boolean {
		this.#assertAlive();
		const result = this._exports.qjs_execute_pending_job();
		if (result < 0) this._throwPending();
		return result > 0;
	}

	/** Runs promise jobs until none is pending, or `maxJobs` of them ran. Returns the number of jobs that ran. */
	executePendingJobs(maxJobs = Number.POSITIVE_INFINITY): number {
		let count = 0;
		while (count < maxJobs && this.executePendingJob()) count++;
		return count;
	}

	/**
	 * Runs `fn` with a handle scope: the handles created during the call are disposed when it returns, except those
	 * passed to `scope.escape()`. Scopes nest.
	 */
	withScope<T>(fn: (scope: HandleScope) => T): T {
		this.#assertAlive();
		const enclosing = this.#scope;
		const tracked = new Set<JSValueHandle>();
		this.#scope = tracked;
		const scope: HandleScope = {
			escape: (handle) => {
				if (tracked.delete(handle)) enclosing?.add(handle);
				return handle;
			},
		};
		try {
			return fn(scope);
		} finally {
			this.#scope = enclosing;
			for (const handle of tracked) handle.dispose();
		}
	}

	/**
	 * Releases the VM and every handle that belongs to it. A VM whose instance trapped (e.g. the host's stack ran out
	 * while it ran) is in an undefined state: it must not be used again, and disposing it only drops the instance.
	 */
	dispose(): void {
		if (this.#disposed) return;
		try {
			for (const handle of this.#owned) handle.dispose();
			for (const handle of this.#singletons.values()) this._exports.qjs_free_value(handle.ptr);
			this._exports.qjs_destroy();
		} catch (error) {
			if (!(error instanceof WebAssembly.RuntimeError)) throw error;
		} finally {
			this.#owned.clear();
			this.#singletons.clear();
			this.#functions.clear();
			this.#disposed = true;
		}
	}

	[Symbol.dispose](): void {
		this.dispose();
	}

	/** The raw exports of quickjs.wasm. Not a stable API. */
	_getExports(): Exports {
		return this._exports;
	}

	// ---- internals ----

	/** @internal */
	_track(handle: JSValueHandle): void {
		this.#owned.add(handle);
		this.#scope?.add(handle);
	}

	/** @internal */
	_untrack(handle: JSValueHandle): void {
		this.#owned.delete(handle);
	}

	/** @internal wraps a box returned by the engine */
	_wrap(ptr: number): JSValueHandle {
		if (ptr === 0) throw new Error("quickjs.wasm: out of memory");
		return new JSValueHandle(this, ptr);
	}

	/** @internal wraps a box, throwing the pending exception if it holds one */
	_check(ptr: number): JSValueHandle {
		if (ptr === 0) throw new Error("quickjs.wasm: out of memory");
		if (this._exports.qjs_typeof(ptr) === TYPE_EXCEPTION) {
			this._exports.qjs_free_value(ptr);
			this._throwPending();
		}
		return new JSValueHandle(this, ptr);
	}

	/** @internal throws the pending exception as a JSException */
	_throwPending(): never {
		const uncatchable = this._exports.qjs_is_uncatchable_exception() !== 0;
		const handle = this._wrap(this._exports.qjs_get_exception());
		// the exception outlives scopes: it is released with the exception or the VM
		this.#scope?.delete(handle);
		throw new JSException(handle, uncatchable);
	}

	/** @internal */
	_toString(handle: JSValueHandle): string {
		this.#assertAlive();
		const exports = this._exports;
		const ptr = exports.qjs_to_string(handle.ptr);
		if (ptr === 0) this._throwPending();
		try {
			const bytes = this.#view().subarray(ptr, ptr + exports.qjs_last_length());
			try {
				return this.#decoder.decode(bytes);
			} catch {
				return decodeWtf8(bytes);
			}
		} finally {
			exports.qjs_free_cstring(ptr);
		}
	}

	/** @internal `String(value)` that never throws (for error reporting) */
	_safeToString(handle: JSValueHandle): string {
		try {
			return this._toString(handle);
		} catch (error) {
			if (error instanceof JSException) error.dispose();
			return `<${handle.typeof}>`;
		}
	}

	/** @internal a property as a string, undefined if missing or if reading it throws */
	_readStringProp(handle: JSValueHandle, name: string): string | undefined {
		try {
			return handle.getProp(name).consume((value) => (value.isUndefined ? undefined : this._safeToString(value)));
		} catch (error) {
			if (error instanceof JSException) error.dispose();
			return undefined;
		}
	}

	/** @internal a zero terminated UTF-8 copy of `text` in the memory; free `ptr` with qjs_free */
	_writeString(text: string): { ptr: number; len: number } {
		const bytes = encodeWtf8(text, this.#encoder);
		const ptr = this._exports.qjs_malloc(bytes.length + 1);
		if (ptr === 0) throw new Error("quickjs.wasm: out of memory");
		const view = this.#view();
		view.set(bytes, ptr);
		view[ptr + bytes.length] = 0;
		return { ptr, len: bytes.length };
	}

	#view(): Uint8Array {
		if (this.#bytes.buffer !== this.#memory.buffer) this.#bytes = new Uint8Array(this.#memory.buffer);
		return this.#bytes;
	}

	#readCString(ptr: number): string {
		const view = this.#view();
		const end = view.indexOf(0, ptr);
		return this.#decoder.decode(view.subarray(ptr, end));
	}

	/** an array of the handles' pointers in the memory; free with qjs_free */
	#writePointers(handles: JSValueHandle[]): number {
		const ptr = this._exports.qjs_malloc(handles.length * 4);
		if (ptr === 0) throw new Error("quickjs.wasm: out of memory");
		const view = new DataView(this.#memory.buffer);
		for (let i = 0; i < handles.length; i++) view.setUint32(ptr + i * 4, handles[i].ptr, true);
		return ptr;
	}

	#singleton(name: string, create: () => number): JSValueHandle {
		this.#assertAlive();
		let handle = this.#singletons.get(name);
		if (!handle) {
			const ptr = create();
			if (ptr === 0) throw new Error("quickjs.wasm: out of memory");
			handle = new JSValueHandle(this, ptr, HANDLE_SINGLETON);
			this.#singletons.set(name, handle);
		}
		return handle;
	}

	#assertAlive(): void {
		if (this.#disposed) throw new Error("The QuickJS VM is disposed");
	}

	#interrupt(): number {
		const handler = this.#interruptHandler;
		if (!handler) return 0;
		try {
			return handler() ? 1 : 0;
		} catch {
			return 1;
		}
	}

	/** Called by the engine for functions made with newFunction(); returns a box or 0 after throwing. */
	#hostCall(id: number, thisPtr: number, argc: number, argv: number): number {
		const exports = this._exports;
		const fn = this.#functions.get(id);
		if (!fn) {
			this.#throwIntoVm(new Error("Host function is not available"));
			return 0;
		}
		const thisHandle = new JSValueHandle(this, thisPtr, HANDLE_BORROWED);
		const args: JSValueHandle[] = [];
		for (let i = 0; i < argc; i++) args.push(new JSValueHandle(this, argv + i * 8, HANDLE_BORROWED));
		try {
			const result = fn.call(thisHandle, ...args);
			return exports.qjs_dup_value((result ?? this.undefined).ptr);
		} catch (error) {
			this.#throwIntoVm(error);
			return 0;
		}
	}

	#throwIntoVm(error: unknown): void {
		if (error instanceof JSException && error.handle.vm === this && !error.handle.disposed) {
			this._exports.qjs_throw(error.handle.ptr, error.uncatchable ? 1 : 0);
			error.dispose();
			return;
		}
		let handle: JSValueHandle;
		try {
			handle = this.newError(error instanceof Error ? error : String(error));
		} catch (inner) {
			// out of memory: throw null rather than let a host exception unwind the engine
			if (inner instanceof JSException) inner.dispose();
			this._exports.qjs_throw(this.null.ptr, 0);
			return;
		}
		this._exports.qjs_throw(handle.ptr, 0);
		handle.dispose();
	}
}
