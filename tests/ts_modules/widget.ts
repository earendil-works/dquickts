// a value module whose exports are only used as types by main.ts
import { log } from "./log.js";
log("widget");

export class Widget { size = 1 }
export default class DefaultWidget {}
