import type { Point } from "./types.js";
import { Id } from "./types";
import { log } from "./log.js";
log("lib");

export function norm(p: Point, id: Id): string {
    return id + ":" + Math.hypot(p.x, p.y);
}
export const enum Flags { None = 0, A = 1 << 0, B = 1 << 1, AB = A | B }
export enum Dir { Up = 1, Down, Left = "L" }
export namespace Geo {
    export const origin = { x: 0, y: 0 };
    export function dist(p: Point): number { return Math.hypot(p.x - origin.x, p.y - origin.y); }
}
export import Origin = Geo.origin;
export type { Point } from "./types.js";
export type * as AllTypes from "./types.js";
export { Point as Pt };
export interface Local { z: number }
export { Local, Local as Renamed };
export default class Thing {
    constructor(public readonly n: number) {}
}
