// module B: top-level await, re-exports and an import cycle with C
import { counter, increment } from "./a.mjs";
export * from "./a.mjs";
export * as nsA from "./a.mjs";
export { label as aLabel } from "./a.mjs";

print("b start", counter);
export let progress = "b-started";
let ticks = 0;
for (let i = 0; i < 3; i++) {
    await null;
    ticks++;
    increment(10);
}
progress = "b-done after " + ticks + " ticks";
export function fromB() {
    return "fromB sees counter " + counter;
}
export default "default-b";
print("b end", counter);
