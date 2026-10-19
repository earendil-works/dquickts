/* Run without -m: this file has no 'import' or 'export' statement and is
   detected as a module because it uses top-level await (ng bug832.js). */
const { assert } = await import("./assert.js");

assert(this, undefined);

/* ng dynamic_import_rejection_handled.js */
const error = await import("./fixture_throwing_module.js").then(() => "ok", e => e);
assert(error.message, "thrown by the module");
