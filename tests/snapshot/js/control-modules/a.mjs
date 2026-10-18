// module A: live bindings mutated by its own functions
export let counter = 0;
export function increment(n = 1) {
    counter += n;
    return counter;
}
export const label = "A";
export const meta = { url: import.meta.url, main: import.meta.main };
export class Thing {
    static created = 0;
    constructor() { Thing.created++; }
}
let hidden = "hidden-a";
export { hidden as renamed };
print("a evaluated", import.meta.url, import.meta.main);
