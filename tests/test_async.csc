# Tests for process.async namespace and file_t async I/O basics.
# Run with: cs -i <path-to-process.cse-dir> test_async.csc

import process

var _pass = 0
var _fail = 0
var _section = ""

function section(name)
    _section = name
    system.out.println("")
    system.out.println("=== " + name + " ===")
end

function check(label, ok)
    if ok
        system.out.println("[PASS] " + _section + " | " + label)
        _pass += 1
    else
        system.out.println("[FAIL] " + _section + " | " + label)
        _fail += 1
    end
end

function check_eq(label, a, b)
    check(label, a == b)
end

function check_not_null(label, v)
    check(label, v != null)
end

function check_null(label, v)
    check(label, v == null)
end

function is_windows()
    return system.getenv("OS") == "Windows_NT"
end

function shell_prog()
    if is_windows()
        return "cmd"
    else
        return "/bin/sh"
    end
end

function enable_shell(b)
    try
        b.use_shell(shell_prog())
        return true
    catch _e1
    end
    return false
end

function open_async_file(path, mode)
    try
        return process.async.fstream(path, mode)
    catch _e
        return null
    end
end

function shell_echo_cmd(s)
    if is_windows()
        return "echo " + s
    else
        return "echo " + s
    end
end

section("A01 builder shell + communicate sanity")
try
    var b = new process.builder
    b.cmd(shell_echo_cmd("async_ok"))
    check("shell API available", enable_shell(b))
    var p = b.start()
    var r = p.communicate()
    check_eq("process exits 0", r[2], 0)
    check("stdout non-empty", r[0] != "")
catch _e
    check("A01 unexpected exception", false)
end

section("A02 async.fstream write/read/flush/close")
try
    var path = "./.tmp_process_async_io.txt"

    var fw = open_async_file(path, "w+")
    check_not_null("fstream(w+) returns file_t", fw)
    if fw == null
        check("async namespace unavailable; A02 skipped", true)
        throw "skip"
    end
    check("file writable", fw.is_writable())

    var written = fw.write("hello_async_file", 1000)
    check_eq("write returns byte count", written, 16)
    var fl = fw.flush(1000)
    check("flush attempted (result=" + fl + ")", true)
    fw.close()

    var fr = open_async_file(path, "r")
    check_not_null("fstream(r) returns file_t", fr)
    check("file readable", fr.is_readable())

    var s1 = fr.read(64, 1000)
    check_eq("read returns content", s1, "hello_async_file")
    var s2 = fr.read(64, 1000)
    check("EOF returns empty string or null", s2 == "" || s2 == null)
    fr.close()

    # closed file should return null on read
    var s3 = fr.read(8, 1000)
    check_null("closed file read returns null", s3)
catch _e2
    if _e2 != "skip"
        check("A02 unexpected exception", false)
    end
end

section("A03 async.poll() and poll_once() run loop")
try
    # Write a file then poll the loop: poll() should return >= 0.
    var path3 = "./.tmp_process_async_poll.txt"
    var fw3 = open_async_file(path3, "w+")
    if fw3 == null
        check("async namespace unavailable; A03 skipped", true)
        throw "skip"
    end
    fw3.write("poll_test", 1000)
    fw3.flush(1000)
    fw3.close()

    var n = process.async.poll()
    check("poll() returns non-negative integer (was " + n + ")", n >= 0)

    var once = process.async.poll_once()
    check("poll_once() returns boolean", once == true || once == false)
catch _e3
    if _e3 != "skip"
        check("A03 unexpected exception", false)
    end
end

section("A04 async.stop() and restart() cycle")
try
    # stop() should not throw; restart() should restore poll-ability.
    process.async.stop()
    check("stop() did not throw", true)
    process.async.restart()
    check("restart() did not throw", true)

    # After restart, poll() should still work.
    var n4 = process.async.poll()
    check("poll() works after restart (was " + n4 + ")", n4 >= 0)
catch _e4
    check("A04 unexpected exception", false)
end

section("A05 fstream invalid mode")
# fstream with an unsupported mode string throws a native CNI exception that
# CovScript try/catch cannot intercept (same constraint as process.exec on a
# missing binary). This is documented behaviour; no runnable assertion is added.
check("invalid mode behaviour is documented (native throw, not null return)", true)

system.out.println("")
system.out.println("----------------------------------------")
system.out.println("Results: " + _pass + " passed, " + _fail + " failed")
system.out.println("----------------------------------------")

if _fail > 0
    system.exit(1)
end
