# dquickts for JavaScript

dquickts compiled to WebAssembly, with JavaScript bindings. A VM is one instance of
`quickjs.wasm` (a WASI reactor module, about 850 KB) holding one runtime and one context. It runs
anywhere WebAssembly does: Node, Bun, Deno, browsers, Cloudflare Workers.

```sh
cd js
make setup     # downloads wasi-sdk 32 to ~/.cache/wasi-sdk (once)
make           # builds quickjs.wasm
npm install
npm run build:ts
npm test
```

`make CONFIG_TEMPORAL_INTL=y` includes the experimental Temporal and Intl objects (enable them
with `Intrinsics.TEMPORAL | Intrinsics.INTL`). `make OPT=-O2` is a few percent faster and about
40% larger.

## Usage

```ts
import { readFile } from "node:fs/promises";
import { EvalFlags, QuickJS } from "dquickts";

const wasm = await WebAssembly.compile(await readFile(new URL(import.meta.resolve("dquickts/quickjs.wasm"))));
const vm = await QuickJS.create({
  wasm,
  memoryLimit: 64 * 1024 * 1024,
  interruptHandler: () => Date.now() > deadline,
});

const add = vm.newFunction("add", (a, b) => vm.newNumber(a.toNumber() + b.toNumber()));
vm.global.setProp("add", add);
add.dispose();

const result = vm.evalCode("const n: number = add(2, 3); n", "main.ts", EvalFlags.TYPESCRIPT);
console.log(result.toNumber()); // 5
result.dispose();
vm.dispose();
```

Pass the compiled `WebAssembly.Module` to every `QuickJS.create()`: it compiles once and can be
sent to workers. On Cloudflare, import `dquickts/quickjs.wasm` with Wrangler's `CompiledWasm`
rule.

## API

`QuickJS.create(options)`:

| Option | |
| --- | --- |
| `wasm` | `quickjs.wasm`, compiled or as bytes |
| `memoryLimit` | bytes the engine may allocate; beyond it scripts get a catchable `InternalError: out of memory` |
| `maxStackSize` | the JS stack guard (default and maximum 512 KiB); deep recursion throws a catchable `InternalError: stack overflow` |
| `interruptHandler` | polled while JS runs; returning true throws an uncatchable `InternalError: interrupted` |
| `intrinsics` | `Intrinsics` flags, e.g. `Intrinsics.ALL & ~Intrinsics.WEAK_REF` |
| `timezoneOffset` | `"host"` (default), a fixed offset in minutes (`getTimezoneOffset()` convention), or a function of the time |
| `wasi` | replaces WASI functions: `clock_time_get` (Date, performance, the seed of `Math.random()`), `random_get`, `fd_write` (engine diagnostics) |

`QuickJS`:

- `evalCode(code, filename?, flags?)` with `EvalFlags.TYPESCRIPT`, `MODULE`, `ASYNC` (top-level
  `await`), `STRICT`
- `callFunction(fn, thisVal, ...args)`, `callConstructor(fn, ...args)`
- `newFunction(name, fn, length?)`: `fn(this, ...args)` gets handles valid during the call only
  (`dup()` them to keep them) and returns a handle (still owned by the host) or undefined. A host
  exception becomes an `Error` with the same name and message in the VM.
- `newString`, `newNumber`, `newBoolean`, `newObject`, `newArray`, `newError`, `newJSON(text)`,
  `newValue(hostValue)` (through JSON), `newPromise()`
- `undefined`, `null`, `true`, `false`, `global`: cached handles, never disposed
- `isJobPending()`, `executePendingJob()`, `executePendingJobs(max?)`
- `withScope((scope) => ...)`: disposes the handles created in the callback, except
  `scope.escape(handle)`
- `memoryUsed`, `setMemoryLimit()`, `setInterruptHandler()`, `runGC()`, `versions`, `dispose()`

`JSValueHandle`: `typeof`, `isUndefined`, `isNull`, `isObject`, `isFunction`, `isArray`,
`isError`, `isPromise`, ..., `toString()`, `toNumber()`, `toBoolean()`, `toJSON()`, `dump()`,
`getProp(key)`, `setProp(key, value)`, `promiseState`, `promiseResult`, `dup()`,
`consume(fn)`, `dispose()`. Handles support `using`.

Errors thrown in the VM surface as `JSException`, with the VM's `name`, `message` and `stack`,
the thrown value in `handle`, and `uncatchable` for interruptions.

Strings cross the boundary as WTF-8, so lone surrogates survive in both directions.

## Differences from quickjs-wasi

The API follows the subset of [quickjs-wasi](https://github.com/vercel-labs/quickjs-wasi) that
embedders use most, so most code only changes its import. The differences come from the engine
(Bellard's QuickJS with this fork's additions, not QuickJS-NG) and from a smaller binding:

- Stack overflow is `InternalError: stack overflow`, not `RangeError: Maximum call stack size
  exceeded`. A JS call uses more wasm stack: about 820 frames fit in 256 KiB (1090 with
  quickjs-wasi).
- `Math.random()` uses another generator: the same seed gives other values.
- An exception in a promise job rejects the job's promise, an interruption included. An
  interrupted async function cannot catch the interruption with `try`/`catch`, but its promise
  rejects. Stop running jobs once the interrupt handler returns true.
- `Atomics` and `SharedArrayBuffer` are not available, as with quickjs-wasi.
- `executePendingJob()` and `isJobPending()` are public; `_getExports()` returns different
  exports.
- `versions` has `dquickts` instead of `quickjs-wasi`.
- No module loader, extensions, bytecode, memory snapshots or `unregisterHostCallback()`.
  Snapshots will use the engine's heap snapshots (`JS_WriteSnapshot()`), not copies of the wasm
  memory.
