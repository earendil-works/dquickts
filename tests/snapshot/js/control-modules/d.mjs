// module D: top-level await on a promise resolved by the main module
export let dValue = "d-pending";
dValue = await globalThis.dGate;
print("d evaluated", dValue);
