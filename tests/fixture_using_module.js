/* module top level 'using' and 'await using' (see test_using.js) */
const log = globalThis.usingModuleLog;

using a = { [Symbol.dispose]() { log.push("dispose a"); } };
await using b = {
    async [Symbol.asyncDispose]() {
        await null;
        log.push("dispose b");
    }
};
export const value = "module body";
log.push(value);
