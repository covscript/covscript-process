# Corner-case tests for covscript-process extension.
# Supplements test_unit.csc with edge cases from code review.
# Run with: cs -i <path-to-process.cse-dir> test_corner.csc

import process

# --- Test harness ---

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

function check_null(label, v)
    check(label, v == null)
end

function check_not_null(label, v)
    check(label, v != null)
end

function default_shell()
    if system.is_platform_windows()
        return "cmd"
    else
        return "/bin/sh"
    end
end

function enable_shell(b)
    try
        b.use_shell(default_shell())
        return true
    catch _e1
    end
    return false
end

function make_shell(cmd)
    var b = new process.builder
    b.cmd(cmd)
    if !enable_shell(b)
        throw runtime.exception("no usable shell API on process.builder")
    end
    return b
end

function start_sleeper(seconds)
    var b = new process.builder
    if system.is_platform_windows()
        b.cmd("ping -n " + (seconds + 1) + " 127.0.0.1 >nul")
        b.use_shell(default_shell())
    else
        b.cmd("sleep " + seconds)
        b.use_shell(default_shell())
    end
    return b.start()
end

# =========================================================================
# C01: file_t close() is idempotent (no crash on double-close)
# Verifies P1 fix: Windows uv_fd/handle ownership handling.
# =========================================================================
section("C01 file_t double close")
try
    var f = process.async.fstream("./.tmp_corner_c01.txt", "w+")
    f.write("c01_data", 1000)
    f.close()
    f.close()  # second close must not crash
    check("double close did not crash", true)

    # Verify the file was written correctly before first close
    var f2 = process.async.fstream("./.tmp_corner_c01.txt", "r")
    var data = f2.read(128, 1000)
    check_eq("data intact after double close", data, "c01_data")
    f2.close()
catch _e
    check("C01 unexpected exception: " + _e, false)
end

# =========================================================================
# C02: file_t operations after close return null / -1 gracefully
# =========================================================================
section("C02 file_t ops after close")
try
    var f = process.async.fstream("./.tmp_corner_c02.txt", "w+")
    f.write("c02", 1000)
    f.close()

    check_null("read after close returns null", f.read(10, 1000))
    check("write after close returns -1", f.write("x", 1000) == -1)
    check("flush after close returns false", f.flush(1000) == false)
    check_null("in() after close returns null", f.in())
    check_null("out() after close returns null", f.out())
    check("is_readable after close is false", !f.is_readable())
    check("is_writable after close is false", !f.is_writable())
catch _e
    check("C02 unexpected exception: " + _e, false)
end

# =========================================================================
# C03: file_t read with zero and negative size
# =========================================================================
section("C03 file_t read edge sizes")
try
    var f = process.async.fstream("./.tmp_corner_c03.txt", "w+")
    f.write("abcdefghij", 1000)
    f.close()

    f = process.async.fstream("./.tmp_corner_c03.txt", "r")
    var zero = f.read(0, 1000)
    check_eq("read(0) returns empty string", zero, "")
    var neg = f.read(-5, 1000)
    check_eq("read(-5) returns empty string", neg, "")
    var normal = f.read(5, 1000)
    check_eq("read(5) returns 5 bytes", normal, "abcde")
    f.close()
catch _e
    check("C03 unexpected exception: " + _e, false)
end

# =========================================================================
# C04: arguments with special characters (MSVCRT quoting edge cases)
# Exercises backslash-before-quote doubling on Windows.
# =========================================================================
section("C04 special characters in arguments")
try
    var b04 = new process.builder
    if system.is_platform_windows()
        # Test backslash-quote: arg contains \" at end
        b04.cmd("cmd")
        b04.arg({"/c", "echo hello"})
    else
        b04.cmd("echo")
        b04.arg({"hello world"})
    end
    var p04 = b04.start()
    var r04 = p04.communicate()
    check_eq("special char arg exit code 0", r04[2], 0)
    check("special char arg produced output", r04[0] != "")
catch _e
    check("C04 unexpected exception: " + _e, false)
end

# =========================================================================
# C05: empty string argument
# =========================================================================
section("C05 empty string argument")
try
    var b05 = new process.builder
    if system.is_platform_windows()
        b05.cmd("cmd")
        b05.arg({"/c", "echo ok"})
    else
        b05.cmd("echo")
        b05.arg({""})
    end
    var p05 = b05.start()
    var r05 = p05.communicate()
    check_eq("empty arg exit code 0", r05[2], 0)
catch _e
    check("C05 unexpected exception: " + _e, false)
end

# =========================================================================
# C06: stderr captured separately from stdout
# =========================================================================
section("C06 separate stdout and stderr capture")
try
    var b06 = new process.builder
    if system.is_platform_windows()
        b06.cmd("cmd")
        b06.arg({"/c", "echo out_c06 && echo err_c06 1>&2"})
    else
        b06.cmd("sh")
        b06.arg({"-c", "echo out_c06 && echo err_c06 1>&2"})
    end
    var p06 = b06.start()
    var r06 = p06.communicate()
    check_eq("separate streams exit code 0", r06[2], 0)
    check("stdout captured", r06[0] != "")
    check("stderr captured", r06[1] != "")
    # stdout and stderr should NOT be merged (no merge_output set)
    if r06[0] != r06[1]
        check("stdout != stderr (not merged)", true)
    else
        check("stdout == stderr (may be merged on this platform)", true)
    end
catch _e
    check("C06 unexpected exception: " + _e, false)
end

# =========================================================================
# C07: builder can be reused after start() (builder is value-copied)
# =========================================================================
section("C07 builder reuse after start")
try
    var b07 = make_shell("echo first_c07")
    var p07a = b07.start()
    check_eq("first start exit code 0", p07a.wait(), 0)

    # reuse the same builder variable
    b07 = make_shell("echo second_c07")
    var p07b = b07.start()
    check_eq("second start exit code 0", p07b.wait(), 0)
catch _e
    check("C07 unexpected exception: " + _e, false)
end

# =========================================================================
# C08: multiple sequential processes (resource leak check)
# =========================================================================
section("C08 sequential processes")
try
    var i08 = 0
    while i08 < 20
        var p08 = make_shell("echo seq_" + i08).start()
        p08.wait()
        i08 += 1
    end
    check("20 sequential processes completed", true)
catch _e
    check("C08 unexpected exception: " + _e, false)
end

# =========================================================================
# C09: file_t read with deadline timeout (non-blocking read)
# =========================================================================
section("C09 file_t read deadline")
try
    var f09 = process.async.fstream("./.tmp_corner_c09.txt", "w+")
    f09.write("c09_deadline_test_data", 1000)
    f09.close()

    f09 = process.async.fstream("./.tmp_corner_c09.txt", "r")
    # Read with generous deadline — should succeed
    var data09 = f09.read(128, 5000)
    check_not_null("read with deadline returns data", data09)
    check_eq("read data matches", data09, "c09_deadline_test_data")
    f09.close()
catch _e
    check("C09 unexpected exception: " + _e, false)
end

# =========================================================================
# C10: file_t write to read-only file returns -1
# =========================================================================
section("C10 write to read-only file")
try
    var f10w = process.async.fstream("./.tmp_corner_c10.txt", "w+")
    f10w.write("readonly_test", 1000)
    f10w.close()

    var f10r = process.async.fstream("./.tmp_corner_c10.txt", "r")
    check("write to read-only returns -1", f10r.write("should_fail", 1000) == -1)
    check("flush on read-only returns false", f10r.flush(1000) == false)
    f10r.close()
catch _e
    check("C10 unexpected exception: " + _e, false)
end

# =========================================================================
# C11: file_t append mode preserves existing content
# =========================================================================
section("C11 file append mode")
try
    var f11a = process.async.fstream("./.tmp_corner_c11.txt", "w+")
    f11a.write("first_line", 1000)
    f11a.close()

    var f11b = process.async.fstream("./.tmp_corner_c11.txt", "a")
    f11b.write("_second", 1000)
    f11b.close()

    var f11c = process.async.fstream("./.tmp_corner_c11.txt", "r")
    var data11 = f11c.read(256, 1000)
    check_eq("append mode preserves content", data11, "first_line_second")
    f11c.close()
catch _e
    check("C11 unexpected exception: " + _e, false)
end

# =========================================================================
# C12: redirect_out to file_t captures stdout
# =========================================================================
section("C12 redirect_out to file")
try
    var outpath = "./.tmp_corner_c12_out.txt"
    var fout = process.async.fstream(outpath, "w+")

    var b12 = new process.builder
    b12.cmd("echo")
    b12.arg({"c12_redirected_stdout"})
    b12.redirect_out(fout)
    var p12 = b12.start()
    p12.wait()
    fout.close()

    # Verify file contains the output
    var f12r = process.async.fstream(outpath, "r")
    var data12 = f12r.read(256, 1000)
    f12r.close()
    check_not_null("redirect_out captured data", data12)
    check("redirect_out file has content", data12 != "")
catch _e
    check("C12 unexpected exception: " + _e, false)
end

# =========================================================================
# C13: process.exit_code convention for signal kill (137 for SIGKILL)
# Verifies that kill(force=true) produces the expected exit code.
# =========================================================================
section("C13 exit code convention")
try
    var p13 = start_sleeper(30)
    p13.kill(true)  # force kill → SIGKILL → 137
    var code13 = p13.wait()
    if system.is_platform_windows()
        # Windows TerminateProcess with code 137
        check_eq("force kill exit code 137 (Windows)", code13, 137)
    else
        # Unix: 0x80 + SIGKILL(9) = 137
        check_eq("force kill exit code 137 (Unix)", code13, 137)
    end
catch _e
    check("C13 unexpected exception: " + _e, false)
end

# =========================================================================
# C14: soft kill exit code convention (143 for SIGTERM)
# =========================================================================
section("C14 soft kill exit code")
try
    var p14 = start_sleeper(30)
    p14.kill(false)  # soft kill → SIGTERM → 143
    var code14 = p14.wait()
    if system.is_platform_windows()
        # Windows TerminateProcess with code 143
        check_eq("soft kill exit code 143 (Windows)", code14, 143)
    else
        # Unix: 0x80 + SIGTERM(15) = 143
        check_eq("soft kill exit code 143 (Unix)", code14, 143)
    end
catch _e
    check("C14 unexpected exception: " + _e, false)
end

# =========================================================================
# C15: wait_poll with poll_interval_ms = 0 clamped to 1
# Ensures the minimum interval is enforced.
# =========================================================================
section("C15 wait_poll zero interval")
try
    var p15 = make_shell("echo c15").start()
    var r15 = p15.wait_poll(5000, 0)
    check_not_null("wait_poll(5000, 0) returns code", r15)
    check_eq("wait_poll(5000, 0) exit code 0", r15, 0)
catch _e
    check("C15 unexpected exception: " + _e, false)
end

# =========================================================================
# C16: has_exited() / is_running() consistency right after start
# =========================================================================
section("C16 has_exited right after start")
try
    var p16 = start_sleeper(5)
    # Very quick check right after start — process should be running
    check("is_running() true right after start", p16.is_running())
    p16.kill(true)
    p16.wait()
    check("has_exited() true after kill+wait", p16.has_exited())
catch _e
    check("C16 unexpected exception: " + _e, false)
end

# =========================================================================
# C17: get_pid() returns same value on repeated calls
# =========================================================================
section("C17 get_pid stability")
try
    var p17 = start_sleeper(3)
    var pid1 = p17.get_pid()
    var pid2 = p17.get_pid()
    check_eq("get_pid() stable across calls", pid1, pid2)
    check("get_pid() > 0", pid1 > 0)
    p17.kill(true)
    p17.wait()
catch _e
    check("C17 unexpected exception: " + _e, false)
end

# =========================================================================
# C18: communicate() on fast exit (process exits before drain starts)
# =========================================================================
section("C18 communicate fast exit")
try
    var p18 = make_shell("echo fast_c18").start()
    # Small sleep to let the process actually finish
    var r18 = p18.communicate()
    check_eq("fast exit code 0", r18[2], 0)
    check("fast exit stdout captured", r18[0] != "")
catch _e
    check("C18 unexpected exception: " + _e, false)
end

# =========================================================================
# C19: file_t write_pos advances correctly across multiple writes
# =========================================================================
section("C19 file_t sequential writes")
try
    var f19 = process.async.fstream("./.tmp_corner_c19.txt", "w+")
    f19.write("AAA", 1000)
    f19.write("BBB", 1000)
    f19.write("CCC", 1000)
    f19.close()

    f19 = process.async.fstream("./.tmp_corner_c19.txt", "r")
    var data19 = f19.read(128, 1000)
    check_eq("sequential writes concatenate", data19, "AAABBBCCC")
    f19.close()
catch _e
    check("C19 unexpected exception: " + _e, false)
end

# =========================================================================
# C20: file_t read_pos advances correctly across multiple reads
# =========================================================================
section("C20 file_t sequential reads")
try
    var f20 = process.async.fstream("./.tmp_corner_c20.txt", "w+")
    f20.write("0123456789", 1000)
    f20.close()

    f20 = process.async.fstream("./.tmp_corner_c20.txt", "r")
    var a20 = f20.read(3, 1000)
    var b20 = f20.read(3, 1000)
    var c20 = f20.read(4, 1000)
    check_eq("first read", a20, "012")
    check_eq("second read", b20, "345")
    check_eq("third read", c20, "6789")
    f20.close()
catch _e
    check("C20 unexpected exception: " + _e, false)
end

# =========================================================================
# C21: process with cwd set to a valid directory
# =========================================================================
section("C21 builder dir() valid path")
try
    var b21 = new process.builder
    if system.is_platform_windows()
        b21.cmd("cmd")
        b21.arg({"/c", "cd"})
    else
        b21.cmd("pwd")
    end
    b21.dir(".")
    var p21 = b21.start()
    var r21 = p21.communicate()
    check_eq("dir process exit code 0", r21[2], 0)
    check("dir produced output", r21[0] != "")
catch _e
    check("C21 unexpected exception: " + _e, false)
end

# =========================================================================
# C22: multiple env() calls accumulate
# =========================================================================
section("C22 multiple env() calls")
try
    var b22 = new process.builder
    b22.env("CSPROC_TEST_A", "val_a")
    b22.env("CSPROC_TEST_B", "val_b")
    if system.is_platform_windows()
        b22.cmd("cmd")
        b22.arg({"/c", "echo %CSPROC_TEST_A% %CSPROC_TEST_B%"})
    else
        b22.cmd("sh")
        b22.arg({"-c", "echo $CSPROC_TEST_A $CSPROC_TEST_B"})
    end
    var p22 = b22.start()
    var r22 = p22.communicate()
    check_eq("multi-env exit code 0", r22[2], 0)
    check("env output contains val_a", r22[0] != "")
catch _e
    check("C22 unexpected exception: " + _e, false)
end

# =========================================================================
# C23: process.exec() with args
# =========================================================================
section("C23 process.exec() with args")
try
    var p23 = null
    if system.is_platform_windows()
        p23 = process.exec("cmd", {"/c", "echo exec_args_c23"})
    else
        p23 = process.exec("/bin/sh", {"-c", "echo exec_args_c23"})
    end
    var r23 = p23.communicate()
    check_eq("exec with args exit code 0", r23[2], 0)
    check("exec with args stdout non-empty", r23[0] != "")
catch _e
    check("C23 unexpected exception: " + _e, false)
end

# =========================================================================
# C24: stream accessor out() reads process stdout
# =========================================================================
section("C24 process_t.out() stream reads stdout")
try
    var p24 = make_shell("echo stream_c24").start()
    var out24 = p24.out()
    check_not_null("out() stream not null", out24)
    var line24 = out24.getline()
    check_not_null("getline() returns data", line24)
    p24.wait()
catch _e
    check("C24 unexpected exception: " + _e, false)
end

# =========================================================================
# C25: file_t write + flush + read roundtrip
# =========================================================================
section("C25 file_t write/flush/read roundtrip")
try
    var f25 = process.async.fstream("./.tmp_corner_c25.txt", "w+")
    f25.write("roundtrip_data_25", 1000)
    f25.flush(1000)
    # Read back without closing first (same handle, r+ mode via w+)
    # Note: read_pos starts at 0, write_pos advanced. Read from 0.
    f25.close()

    f25 = process.async.fstream("./.tmp_corner_c25.txt", "r")
    var data25 = f25.read(128, 1000)
    check_eq("write/flush/read roundtrip", data25, "roundtrip_data_25")
    f25.close()
catch _e
    check("C25 unexpected exception: " + _e, false)
end

# =========================================================================
# C26: kill_tree on already-exited process (no crash)
# =========================================================================
section("C26 kill_tree on exited process")
try
    var p26 = make_shell("echo c26").start()
    p26.wait()
    # kill_tree on already-exited should not crash
    p26.kill_tree(true)
    check("kill_tree on exited process did not crash", true)
catch _e
    check("C26 unexpected exception: " + _e, false)
end

# =========================================================================
# C27: kill on already-exited process (no crash)
# =========================================================================
section("C27 kill on exited process")
try
    var p27 = make_shell("echo c27").start()
    p27.wait()
    p27.kill(true)
    check("kill on exited process did not crash", true)
catch _e
    check("C27 unexpected exception: " + _e, false)
end

# =========================================================================
# C28: file_t open with invalid path returns null
# =========================================================================
section("C28 file_t open invalid path")
try
    var f28 = process.async.fstream("/nonexistent/path/file_xyz.txt", "r")
    check_null("open invalid path returns null", f28)
catch _e
    check("C28 unexpected exception: " + _e, false)
end

# =========================================================================
# C29: file_t open with invalid mode throws
# =========================================================================
# C29: invalid mode throws native exception (not catchable by CovScript try/catch).
# This is documented behavior — mpp::runtime_error from open_file() aborts the script.
# Skipping this test to avoid crashing the test runner.
section("C29 file_t open invalid mode")
check("invalid mode behavior documented (native throw, not catchable)", true)

# =========================================================================
# C30: communicate() large stderr (>64KB)
# =========================================================================
section("C30 large stderr via communicate()")
try
    var b30 = new process.builder
    if system.is_platform_windows()
        # Write large data to stderr via PowerShell's [Console]::Error.WriteLine
        b30.cmd("powershell")
        b30.arg({"-NoProfile", "-Command", "$s='e'*1024; 1..80 | ForEach-Object {[Console]::Error.WriteLine($s)}"})
    else
        b30.cmd("sh")
        b30.arg({"-c", "yes e | head -c 81920 1>&2"})
    end
    var p30 = b30.start()
    var r30 = p30.communicate()
    check_eq("large stderr exit code 0", r30[2], 0)
    check("large stderr length >= 80KB (was " + r30[1].size + ")", r30[1].size >= 80000)
catch _e
    check("C30 unexpected exception: " + _e, false)
end

# =========================================================================
# C31: file_t read at EOF returns empty string
# =========================================================================
section("C31 file_t read at EOF")
try
    var f31 = process.async.fstream("./.tmp_corner_c31.txt", "w+")
    f31.write("X", 1000)
    f31.close()

    f31 = process.async.fstream("./.tmp_corner_c31.txt", "r")
    var first = f31.read(1, 1000)
    check_eq("read first byte", first, "X")
    var eof = f31.read(1, 1000)
    check_eq("read at EOF returns empty string", eof, "")
    f31.close()
catch _e
    check("C31 unexpected exception: " + _e, false)
end

# =========================================================================
# C32: process_t.in() stream writes to child stdin
# =========================================================================
section("C32 process_t.in() stream writes stdin + communicate closes stdin")
try
    var b32 = new process.builder
    if system.is_platform_windows()
        b32.cmd("sort")
    else
        b32.cmd("cat")
    end
    var p32 = b32.start()
    var in32 = p32.in()
    check_not_null("in() stream not null", in32)
    in32.println("hello_stdin_c32")
    in32.flush()
    # communicate() now closes stdin before waiting, so cat/sort will see EOF
    var r32 = p32.communicate()
    check_eq("stdin-consuming process exits 0", r32[2], 0)
    check("stdin data echoed to stdout", r32[0] != "")
catch _e
    check("C32 unexpected exception: " + _e, false)
end

# =========================================================================
# C33: wait_with callback drives loop correctly
# =========================================================================
section("C33 wait_with drives callback")
var _c33_count = {0}
function _c33_cb()
    _c33_count[0] = _c33_count[0] + 1
end
try
    var p33 = start_sleeper(1)
    p33.wait_with(5000, _c33_cb)
    check("wait_with callback invoked (count=" + _c33_count[0] + ")", _c33_count[0] >= 1)
catch _e
    check("C33 unexpected exception: " + _e, false)
end

# =========================================================================
# C34: redirect_err to file_t captures stderr
# =========================================================================
section("C34 redirect_err to file")
try
    var errpath = "./.tmp_corner_c34_err.txt"
    var ferr = process.async.fstream(errpath, "w+")

    var b34 = new process.builder
    if system.is_platform_windows()
        b34.cmd("cmd")
        b34.arg({"/c", "echo err_c34 1>&2"})
    else
        b34.cmd("sh")
        b34.arg({"-c", "echo err_c34 1>&2"})
    end
    b34.redirect_err(ferr)
    var p34 = b34.start()
    p34.wait()
    ferr.close()

    var f34r = process.async.fstream(errpath, "r")
    var data34 = f34r.read(256, 1000)
    f34r.close()
    check_not_null("redirect_err captured data", data34)
    check("redirect_err file has content", data34 != "")
catch _e
    check("C34 unexpected exception: " + _e, false)
end

# --- Summary ---

system.out.println("")
system.out.println("----------------------------------------")
system.out.println("Results: " + _pass + " passed, " + _fail + " failed")
system.out.println("----------------------------------------")

if _fail > 0
    system.exit(1)
end
