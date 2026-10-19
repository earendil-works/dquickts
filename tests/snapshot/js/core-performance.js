// performance.now() and performance.timeOrigin across hibernation: the
// clock of the restored program continues from its saved value, also
// in another process, and the time origin is kept.
var origin = performance.timeOrigin;
var last = performance.now();
var monotonic = typeof last === "number" && last >= 0;
var stable = typeof origin === "number" && origin > 1e12;
var checks = 0;

function check() {
    var t = performance.now();
    if (!(t >= last))
        monotonic = false;
    last = t;
    if (performance.timeOrigin !== origin)
        stable = false;
    checks++;
}

function spin(n) {
    for (let i = 0; i < n; i++) {
        if (i % 1000 === 0)
            check();
    }
}

spin(200000);
var p = Promise.resolve();
for (let i = 0; i < 4; i++)
    p = p.then(() => spin(50000));

function report() {
    print("monotonic", monotonic, "origin kept", stable, "checks", checks);
    print("object", String(performance), typeof performance.now,
          Object.getOwnPropertyNames(performance).sort().join());
}
p.then(report);
