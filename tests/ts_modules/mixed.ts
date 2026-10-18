// 'OnlyType' has no runtime binding: importing it with 'usedValue'
// only works because the unused binding is elided
import { log } from "./log.js";
log("mixed");

export const usedValue = 42;
export interface OnlyType { a: number }
