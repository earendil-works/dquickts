// Only types: importing these names, even without 'import type', must
// not load this module (the log entry would show it).
import { log } from "./log.js";
log("types");

export interface Point { x: number; y: number }
export type Id = number | string;
export declare const declared: number;
export namespace Shapes { export interface Circle { r: number } }
