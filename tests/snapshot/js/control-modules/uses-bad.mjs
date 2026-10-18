// imports a module whose evaluation failed: its evaluation fails with the
// saved exception of that module
import "./bad.mjs";
export const ok = true;
print("uses-bad evaluated");
