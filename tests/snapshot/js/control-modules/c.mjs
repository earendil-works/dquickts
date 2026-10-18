// module C: loaded with import(); depends on A and on D (top-level await)
import { counter } from "./a.mjs";
import { dValue } from "./d.mjs";
export const value = 42;
export default function () {
    return "c-default sees counter " + counter + ", " + dValue;
}
export function readA() {
    return counter;
}
print("c evaluated", import.meta.url, import.meta.main, dValue);
