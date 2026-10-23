/**
 * The WASI functions quickjs.wasm imports (`wasi_snapshot_preview1`).
 *
 * The engine only needs clocks (`Date.now()`, `performance.now()` and the seed of `Math.random()`), random bytes
 * (libc start-up) and stdout/stderr (diagnostics of the engine). Files are not available.
 */

/** The linear memory of the instance; `buffer` changes when the memory grows. */
export interface WasmMemory {
	readonly buffer: ArrayBuffer;
}

// biome-ignore lint/suspicious/noExplicitAny: WASI functions take raw numbers and bigints.
export type WasiFunction = (...args: any[]) => number;

/**
 * Replaces WASI functions. Called once per VM with its memory, before the instance exists (read `memory.buffer` only
 * inside the functions). Every function receives raw pointers into the memory and returns a WASI errno (0 is
 * success).
 *
 * ```ts
 * // a fixed clock and no output
 * wasi: (memory) => ({
 *   clock_time_get(_id, _precision, resultPtr) {
 *     new DataView(memory.buffer).setBigUint64(resultPtr, 1_700_000_000_000n * 1_000_000n, true);
 *     return 0;
 *   },
 *   fd_write(_fd, iovs, iovsLen, nwritten) { ... },
 * })
 * ```
 */
export type WasiOverrides = (memory: WasmMemory) => Record<string, WasiFunction>;

const ERRNO_SUCCESS = 0;
const ERRNO_BADF = 8;
const ERRNO_INVAL = 28;
const ERRNO_NOSYS = 52;

const CLOCK_REALTIME = 0;
const CLOCK_MONOTONIC = 1;
const CLOCK_PROCESS_CPUTIME = 2;
const CLOCK_THREAD_CPUTIME = 3;

interface ConsoleLike {
	log(text: string): void;
	error(text: string): void;
}

interface WritableLike {
	write(text: string): unknown;
}

interface ProcessLike {
	stdout?: WritableLike;
	stderr?: WritableLike;
}

function now(): bigint {
	return BigInt(Date.now()) * 1_000_000n;
}

function monotonic(): bigint {
	const performance = (globalThis as { performance?: { now(): number } }).performance;
	if (!performance) return now();
	return BigInt(Math.round(performance.now() * 1_000_000));
}

/** Default WASI functions: real clocks, `crypto.getRandomValues()`, output to the host's stdout and stderr. */
export function createWasi(memory: WasmMemory): Record<string, WasiFunction> {
	const decoder = new TextDecoder();
	return {
		clock_time_get(clockId: number, _precision: bigint, resultPtr: number): number {
			let time: bigint;
			switch (clockId) {
				case CLOCK_REALTIME:
					time = now();
					break;
				case CLOCK_MONOTONIC:
				case CLOCK_PROCESS_CPUTIME:
				case CLOCK_THREAD_CPUTIME:
					time = monotonic();
					break;
				default:
					return ERRNO_INVAL;
			}
			new DataView(memory.buffer).setBigUint64(resultPtr, time, true);
			return ERRNO_SUCCESS;
		},
		clock_res_get(_clockId: number, resultPtr: number): number {
			new DataView(memory.buffer).setBigUint64(resultPtr, 1000n, true);
			return ERRNO_SUCCESS;
		},
		fd_write(fd: number, iovsPtr: number, iovsLen: number, nwrittenPtr: number): number {
			if (fd !== 1 && fd !== 2) return ERRNO_BADF;
			const view = new DataView(memory.buffer);
			let text = "";
			let written = 0;
			for (let i = 0; i < iovsLen; i++) {
				const ptr = view.getUint32(iovsPtr + i * 8, true);
				const len = view.getUint32(iovsPtr + i * 8 + 4, true);
				text += decoder.decode(new Uint8Array(memory.buffer, ptr, len));
				written += len;
			}
			const process = (globalThis as { process?: ProcessLike }).process;
			const stream = fd === 1 ? process?.stdout : process?.stderr;
			if (stream) stream.write(text);
			else {
				const console = (globalThis as { console?: ConsoleLike }).console;
				if (fd === 1) console?.log(text);
				else console?.error(text);
			}
			view.setUint32(nwrittenPtr, written, true);
			return ERRNO_SUCCESS;
		},
		fd_close(): number {
			return ERRNO_BADF;
		},
		// No files: no preopened directories, so every path lookup fails (imported by builds with Temporal and Intl).
		fd_prestat_get(): number {
			return ERRNO_BADF;
		},
		fd_prestat_dir_name(): number {
			return ERRNO_BADF;
		},
		fd_read(): number {
			return ERRNO_BADF;
		},
		path_open(): number {
			return ERRNO_BADF;
		},
		path_readlink(): number {
			return ERRNO_BADF;
		},
		fd_seek(): number {
			return ERRNO_BADF;
		},
		fd_fdstat_get(fd: number, statPtr: number): number {
			if (fd !== 1 && fd !== 2) return ERRNO_BADF;
			const view = new DataView(memory.buffer);
			view.setUint8(statPtr, 2); // character device
			view.setUint16(statPtr + 2, 0, true);
			view.setBigUint64(statPtr + 8, 0n, true);
			view.setBigUint64(statPtr + 16, 0n, true);
			return ERRNO_SUCCESS;
		},
		random_get(bufPtr: number, bufLen: number): number {
			const bytes = new Uint8Array(memory.buffer, bufPtr, bufLen);
			const crypto = (globalThis as { crypto?: { getRandomValues(array: Uint8Array): unknown } }).crypto;
			if (crypto) {
				// getRandomValues() fills at most 65536 bytes per call
				for (let offset = 0; offset < bufLen; offset += 65536) {
					crypto.getRandomValues(bytes.subarray(offset, Math.min(bufLen, offset + 65536)));
				}
			} else {
				for (let i = 0; i < bufLen; i++) bytes[i] = Math.floor(Math.random() * 256);
			}
			return ERRNO_SUCCESS;
		},
		proc_exit(): number {
			return ERRNO_NOSYS;
		},
		environ_sizes_get(countPtr: number, sizePtr: number): number {
			const view = new DataView(memory.buffer);
			view.setUint32(countPtr, 0, true);
			view.setUint32(sizePtr, 0, true);
			return ERRNO_SUCCESS;
		},
		environ_get(): number {
			return ERRNO_SUCCESS;
		},
	};
}
