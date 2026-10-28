/* Built with qjsc -fno-unused by "make test": the built-ins not named by
   this code are omitted. The names of the omitted ones are built at run
   time so that qjsc does not see them. */
function assert(ok, msg)
{
    if (!ok)
        throw Error("assertion failed: " + msg);
}

function has(name)
{
    return Object.getOwnPropertyNames(globalThis).indexOf(name) >= 0;
}

/* named or needed by a construct: kept */
assert(new Map([[1, 2]]).get(1) === 2, "Map");
assert(/b+/.exec("abbc")[0] === "bb", "regexp literal");
assert(JSON.stringify([1]) === "[1]", "JSON");
assert("A\u030a".normalize() === "\u00c5", "normalize");
async function f() { return 1; }
assert(has("Prom" + "ise"), "an async function keeps Promise");

/* not named: omitted */
["Da" + "te", "Pro" + "xy", "Weak" + "Ref", "Uint8" + "Array",
 "DOM" + "Exception", "at" + "ob", "perfor" + "mance"].forEach(function (name) {
     assert(!has(name), name + " is omitted");
 });

/* no parser: eval() throws */
var threw = false;
try {
    (0, globalThis["ev" + "al"])("1");
} catch (e) {
    threw = true;
}
assert(threw, "eval is omitted");
