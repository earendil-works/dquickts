/* Built by "make test" with qjsc -e -fno-unused -fno-module-loader
   -M examples/fib.so,fib and linked with examples/fib.c: no module can be
   loaded from the disk, they must be found among the embedded modules and
   the C modules. */
import bytes from "./fixture_bytes_module.txt" with { type: "bytes" };
import text from "./fixture_bytes_module.txt" with { type: "text" };
import json from "./fixture_import_attributes.json" with { type: "json" };
import json_no_attr from "./fixture_import_attributes.json";

function assert(ok, msg)
{
    if (!ok)
        throw Error("assertion failed: " + msg);
}

var ns;

/* the embedded modules are found with their import attributes */
assert(bytes instanceof Uint8Array && bytes.length === 4 &&
       bytes[0] === 0x68 && bytes[3] === 0x0a, "bytes module");
assert(text === "h\u00e9\n", "text module");
assert(json.answer === 42, "json module");
assert(json_no_attr.answer === 42, "json module without attributes");

ns = await import("./fixture_import_attributes.json", { with: { type: "json" } });
assert(ns.default === json, "dynamic import of a json module");
ns = await import("./fixture_bytes_module.txt", { with: { type: "bytes" } });
assert(ns.default === bytes, "dynamic import of a bytes module");
