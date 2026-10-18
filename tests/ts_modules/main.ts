import Thing, { norm, Flags, Dir, Geo, Origin, Point, Pt, Local } from "./lib.js";
import { Widget } from "./widget.js";
import DefaultWidget from "./widget.js";
import * as WidgetNS from "./widget.js";
import { usedValue, OnlyType } from "./mixed.js";
import { type Id, type Point as P2 } from "./types.js";
import type * as TypesNS from "./types.js";
import "./side.js";
import { fromExtensionless } from "./ext";
import { fromMts } from "./esm.mjs";
import { entries } from "./log.js";

let w: Widget | DefaultWidget | WidgetNS.Widget | undefined;
let o: OnlyType = { a: 1 };
let p: Point & Pt & P2 & TypesNS.Point = { x: 3, y: 4 };
let l: Local | undefined;

/* folded at compile time: 'Flags' is a const enum of another module, so
   it is imported as a value (cross-module folding needs type info) */
export function results() {
    return {
        log: entries.join(","),
        norm: norm(p, 7 as Id),
        flags: Flags.AB,
        dir: Dir.Down + Dir[2] + Dir.Left,
        dist: Geo.dist(p),
        origin: Origin === Geo.origin,
        thing: new Thing(5).n,
        used: usedValue + o.a,
        ext: fromExtensionless + "/" + fromMts,
        w: w === undefined && l === undefined,
    };
}
