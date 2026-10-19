#! (shebang test)
import * as std from "std";
import * as os from "os";

function assert(actual, expected, message) {
    if (arguments.length == 1)
        expected = true;

    if (Object.is(actual, expected))
        return;

    if (actual !== null && expected !== null
    &&  typeof actual == 'object' && typeof expected == 'object'
    &&  actual.toString() === expected.toString())
        return;

    throw Error("assertion failed: got |" + actual + "|" +
                ", expected |" + expected + "|" +
                (message ? " (" + message + ")" : ""));
}

// load more elaborate version of assert if available
try { std.loadScript("test_assert.js"); } catch(e) {}

/*----------------*/

function test_printf()
{
    assert(std.sprintf("a=%d s=%s", 123, "abc"), "a=123 s=abc");
    assert(std.sprintf("%010d", 123), "0000000123");
    assert(std.sprintf("%x", -2), "fffffffe");
    assert(std.sprintf("%lx", -2), "fffffffffffffffe");
    assert(std.sprintf("%10.1f", 2.1), "       2.1");
    assert(std.sprintf("%*.*f", 10, 2, -2.13), "     -2.13");
    assert(std.sprintf("%#lx", 0x7fffffffffffffffn), "0x7fffffffffffffff");
}

function test_file1()
{
    var f, len, str, size, buf, ret, i, str1;

    f = std.tmpfile();
    str = "hello world\n";
    f.puts(str);

    f.seek(0, std.SEEK_SET);
    str1 = f.readAsString();
    assert(str1 === str);

    f.seek(0, std.SEEK_END);
    size = f.tell();
    assert(size === str.length);

    f.seek(0, std.SEEK_SET);

    buf = new Uint8Array(size);
    ret = f.read(buf.buffer, 0, size);
    assert(ret === size);
    for(i = 0; i < size; i++)
        assert(buf[i] === str.charCodeAt(i));

    f.close();
}

function test_file2()
{
    var f, str, i, size;
    f = std.tmpfile();
    str = "hello world\n";
    size = str.length;
    for(i = 0; i < size; i++)
        f.putByte(str.charCodeAt(i));
    f.seek(0, std.SEEK_SET);
    for(i = 0; i < size; i++) {
        assert(str.charCodeAt(i) === f.getByte());
    }
    assert(f.getByte() === -1);
    f.close();
}

function test_getline()
{
    var f, line, line_count, lines, i;

    lines = ["hello world", "line 1", "line 2" ];
    f = std.tmpfile();
    for(i = 0; i < lines.length; i++) {
        f.puts(lines[i], "\n");
    }

    f.seek(0, std.SEEK_SET);
    assert(!f.eof());
    line_count = 0;
    for(;;) {
        line = f.getline();
        if (line === null)
            break;
        assert(line == lines[line_count]);
        line_count++;
    }
    assert(f.eof());
    assert(line_count === lines.length);

    f.close();
}

function test_popen()
{
    var str, f, fname = "tmp_file.txt";
    var content = "hello world";

    f = std.open(fname, "w");
    f.puts(content);
    f.close();

    /* test loadFile */
    assert(std.loadFile(fname), content);

    /* execute the 'cat' shell command */
    f = std.popen("cat " + fname, "r");
    str = f.readAsString();
    f.close();

    assert(str, content);

    os.remove(fname);
}

function test_open_modes()
{
    var fname = "tmp_file_x.txt", f, err = {};

    os.remove(fname);
    f = std.open(fname, "wx", err);
    assert(f !== null && err.errno === 0);
    f.close();
    /* 'x' fails if the file exists */
    f = std.open(fname, "wx", err);
    assert(f, null);
    assert(err.errno, std.Error.EEXIST);
    os.remove(fname);

    /* popen() only accepts "r" or "w" */
    for (var mode of ["rw", "", "r+"]) {
        try {
            std.popen("true", mode);
            assert(false, true, "popen mode '" + mode + "'");
        } catch(e) {
            assert(e instanceof TypeError);
        }
    }
}

function test_load_file()
{
    var fname = "tmp_load_file.txt", f, str, fds;

    /* larger than the initial buffer used for non seekable files */
    str = "0123456789abcdef".repeat(375);
    f = std.open(fname, "w");
    f.puts(str);
    f.close();
    assert(std.loadFile(fname), str);
    f = std.open(fname, "w");
    f.close();
    assert(std.loadFile(fname), "");
    os.remove(fname);

    /* pipes are not seekable (FreeBSD only has /dev/fd/N for N > 2 with
       fdescfs mounted) */
    fds = os.pipe();
    f = std.fdopen(fds[1], "w");
    f.puts(str);
    f.close();
    if (os.stat("/dev/fd/" + fds[0])[1] === 0)
        assert(std.loadFile("/dev/fd/" + fds[0]), str);
    os.close(fds[0]);

    /* the files of /proc report a size of 0 (os.platform is also "linux"
       on FreeBSD, which has no /proc by default) */
    if (os.stat("/proc/self/status")[1] === 0)
        assert(std.loadFile("/proc/self/status").startsWith("Name:"));
}

function test_ext_json()
{
    var expected, input, obj;
    expected = '{"x":false,"y":true,"z2":null,"a":[1,8,160],"b":"abc\\u000bd","s":"str"}';
    input = `{ "x":false, /*comments are allowed */
               "y":true,  // also a comment
               z2:null, // unquoted property names
               "a":[+1,0o10,0xa0,], // plus prefix, octal, hexadecimal
               "b": "ab\
c\\vd", // multi-line strings, '\v' escape
               "s":'str',} // trailing comma in objects and arrays, single quoted string
            `;
    obj = std.parseExtJSON(input);
    assert(JSON.stringify(obj), expected);

    obj = std.parseExtJSON('[Infinity, +Infinity, -Infinity, NaN, +NaN, -NaN, .1, -.2]');
    assert(obj[0], Infinity);
    assert(obj[1], Infinity);
    assert(obj[2], -Infinity);
    assert(obj[3], NaN);
    assert(obj[4], NaN);
    assert(obj[5], NaN);
    assert(obj[6], 0.1);
    assert(obj[7], -0.2);
}

function test_os()
{
    var fd, fpath, fname, fdir, buf, buf2, i, files, err, fdate, st, link_path;

    const stdinIsTTY = !os.exec(["/bin/sh", "-c", "test -t 0"], { usePath: false });

    assert(os.isatty(0), stdinIsTTY, `isatty(STDIN)`);

    fdir = "test_tmp_dir";
    fname = "tmp_file.txt";
    fpath = fdir + "/" + fname;
    link_path = fdir + "/test_link";

    os.remove(link_path);
    os.remove(fpath);
    os.remove(fdir);

    err = os.mkdir(fdir, 0o755);
    assert(err === 0);

    fd = os.open(fpath, os.O_RDWR | os.O_CREAT | os.O_TRUNC);
    assert(fd >= 0);

    buf = new Uint8Array(10);
    for(i = 0; i < buf.length; i++)
        buf[i] = i;
    assert(os.write(fd, buf.buffer, 0, buf.length) === buf.length);

    assert(os.seek(fd, 0, std.SEEK_SET) === 0);
    buf2 = new Uint8Array(buf.length);
    assert(os.read(fd, buf2.buffer, 0, buf2.length) === buf2.length);

    for(i = 0; i < buf.length; i++)
        assert(buf[i] == buf2[i]);

    if (typeof BigInt !== "undefined") {
        assert(os.seek(fd, BigInt(6), std.SEEK_SET), BigInt(6));
        assert(os.read(fd, buf2.buffer, 0, 1) === 1);
        assert(buf[6] == buf2[0]);
    }

    assert(os.close(fd) === 0);

    [files, err] = os.readdir(fdir);
    assert(err, 0);
    assert(files.indexOf(fname) >= 0);

    fdate = 10000;

    err = os.utimes(fpath, fdate, fdate);
    assert(err, 0);

    [st, err] = os.stat(fpath);
    assert(err, 0);
    assert(st.mode & os.S_IFMT, os.S_IFREG);
    assert(st.mtime, fdate);

    err = os.symlink(fname, link_path);
    assert(err === 0);

    [st, err] = os.lstat(link_path);
    assert(err, 0);
    assert(st.mode & os.S_IFMT, os.S_IFLNK);

    [buf, err] = os.readlink(link_path);
    assert(err, 0);
    assert(buf, fname);

    assert(os.remove(link_path) === 0);

    [buf, err] = os.getcwd();
    assert(err, 0);

    [buf2, err] = os.realpath(".");
    assert(err, 0);

    assert(buf, buf2);

    assert(os.remove(fpath) === 0);

    fd = os.open(fpath, os.O_RDONLY);
    assert(fd < 0);

    assert(os.remove(fdir) === 0);
}

function test_os_tmp()
{
    var dir, err, path, path2, fd, f, st;

    [dir, err] = os.mkdtemp();
    assert(err, 0);
    assert(/^tmp[^\/]{6}$/.test(dir), true, dir);
    [st, err] = os.stat(dir);
    assert(err, 0);
    assert(st.mode & os.S_IFMT, os.S_IFDIR);
    assert(st.mode & 0o777, 0o700);

    /* "XXXXXX" is appended if missing */
    [path, fd] = os.mkstemp(dir + "/f");
    assert(fd >= 0);
    assert(path.startsWith(dir + "/f") && path.length === dir.length + 8,
           true, path);
    [st, err] = os.stat(path);
    assert(err, 0);
    assert(st.mode & 0o777, 0o600);
    f = std.fdopen(fd, "w+");
    f.puts("xyzzy");
    f.seek(0, std.SEEK_SET);
    assert(f.readAsString(), "xyzzy");
    f.close();

    [path2, fd] = os.mkstemp(dir + "/gXXXXXX");
    assert(fd >= 0);
    assert(path2.startsWith(dir + "/g") && path2.length === dir.length + 8,
           true, path2);
    os.close(fd);

    [, fd] = os.mkstemp(dir + "/none/XXXXXX");
    assert(fd, -std.Error.ENOENT);
    [, err] = os.mkdtemp(dir + "/none/XXXXXX");
    assert(err, std.Error.ENOENT);

    assert(os.remove(path), 0);
    assert(os.remove(path2), 0);
    assert(os.remove(dir), 0);
}

function test_exe_path()
{
    var path = os.exePath(), real, err;

    assert(typeof path, "string");
    assert(/\/qjs[^\/]*$/.test(path), true, path);
    /* absolute and canonical */
    [real, err] = os.realpath(path);
    assert(err, 0);
    assert(real, path);
}

function test_os_exec()
{
    var ret, fds, pid, f, status;

    ret = os.exec(["true"]);
    assert(ret, 0);

    ret = os.exec(["/bin/sh", "-c", "exit 1"], { usePath: false });
    assert(ret, 1);

    fds = os.pipe();
    pid = os.exec(["sh", "-c", "echo $FOO"], {
        stdout: fds[1],
        block: false,
        env: { FOO: "hello" },
    } );
    assert(pid >= 0);
    os.close(fds[1]); /* close the write end (as it is only in the child)  */
    f = std.fdopen(fds[0], "r");
    assert(f.getline(), "hello");
    assert(f.getline(), null);
    f.close();
    [ret, status] = os.waitpid(pid, 0);
    assert(ret, pid);
    assert(status & 0x7f, 0); /* exited */
    assert(status >> 8, 0); /* exit code */

    pid = os.exec(["cat"], { block: false } );
    assert(pid >= 0);
    os.kill(pid, os.SIGTERM);
    [ret, status] = os.waitpid(pid, 0);
    assert(ret, pid);
    assert(status !== 0, true, `expect nonzero exit code (got ${status})`);
    assert(status & 0x7f, os.SIGTERM);
}

function test_timer()
{
    var th, i;

    /* just test that a timer can be inserted and removed */
    th = [];
    for(i = 0; i < 3; i++)
        th[i] = os.setTimeout(function () { }, 1000);
    for(i = 0; i < 3; i++)
        os.clearTimeout(th[i]);
}

async function test_interval()
{
    var count = 0, n = 0, id;

    await new Promise((resolve, reject) => {
        id = os.setInterval(() => {
            count++;
            if (count == 3) {
                os.clearInterval(id);
                /* wait for more calls of a cleared interval */
                os.setTimeout(resolve, 20);
            } else if (count > 3) {
                reject(new Error("interval called after clearInterval()"));
            }
        }, 1);
    });
    assert(count, 3);

    /* timeouts and intervals share their ids */
    id = os.setInterval(() => n++, 1);
    os.clearTimeout(id);
    id = os.setTimeout(() => n++, 1);
    os.clearInterval(id);
    await new Promise(resolve => os.setTimeout(resolve, 10));
    assert(n, 0);

    /* an interval does not starve the other timers */
    id = os.setInterval(() => n++, 0);
    await new Promise(resolve => os.setTimeout(resolve, 10));
    os.clearInterval(id);
    assert(n > 0);
}

async function test_module_types()
{
    var ns, ns2;

    /* the 'type' attribute wins over the .json suffix */
    ns = await import("./fixture_text_module.json", { with: { type: "text" } });
    assert(ns.default, '{ "text": "héllo 🌍" }\n');

    ns = await import("./fixture_bytes_module.txt", { with: { type: "bytes" } });
    assert(ns.default instanceof Uint8Array);
    assert(ns.default.join(), "104,195,169,10");
    ns2 = await import("./fixture_bytes_module.txt", { with: { type: "bytes" } });
    assert(ns2.default === ns.default);

    try {
        await import("./fixture_none.txt", { with: { type: "css" } });
        assert(false, true, "unsupported module type");
    } catch(e) {
        assert(e instanceof TypeError, true, e);
    }
}

/* test closure variable handling when freeing asynchronous
   function */
function test_async_gc()
{
    (async function run () {
        let obj = {}

        let done = () => {
            obj
            std.gc();
        }

        Promise.resolve().then(done)

        const p = new Promise(() => {})

        await p
    })();
}

/* check that the promise async rejection handler is not invoked when
   the rejection is handled not too late after the promise
   rejection. */
function test_async_promise_rejection()
{
    var counter = 0;
    var p1, p2, p3;
    p1 = Promise.reject();
    p2 = Promise.reject();
    p3 = Promise.resolve();
    p1.catch(() => counter++);
    p2.catch(() => counter++);
    p3.then(() => counter++)
    os.setTimeout(() => { assert(counter, 3) }, 10);
}

/* ng bug1672: the Array.prototype loops which do not call user code for
   holes must still poll the interrupt handler. A pending SIGINT makes
   the handler of std.evalScript() interrupt the evaluation. */
function test_array_loop_interrupt()
{
    var names = [ "every", "some", "forEach", "filter", "reduce",
                  "reduceRight", "find", "findIndex", "findLast",
                  "findLastIndex", "includes", "indexOf", "lastIndexOf" ];
    os.signal(os.SIGINT, function() { });
    for (var name of names) {
        var args = name.startsWith("reduce") ? "() => 0, 0" :
            (name.endsWith("Of") || name == "includes") ? "1" : "() => false";
        var e = undefined;
        os.kill(os.getpid(), os.SIGINT);
        try {
            std.evalScript("Array.prototype." + name +
                           ".call({length: 2**53 - 1}, " + args + ")");
        } catch (ex) {
            e = ex;
        }
        assert(e instanceof InternalError, true, name);
        assert(e.message, "interrupted", name);
    }
    os.signal(os.SIGINT, null);
}

test_printf();
test_file1();
test_file2();
test_getline();
test_popen();
test_open_modes();
test_load_file();
test_os();
test_os_tmp();
test_exe_path();
test_os_exec();
test_timer();
test_ext_json();
test_async_gc();
test_async_promise_rejection();
test_array_loop_interrupt();
await test_interval();
await test_module_types();

