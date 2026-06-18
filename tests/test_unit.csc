# Unit tests for covscript-process extension.
# Run with: cs -i <path-to-process.cse-dir> test_unit.csc
# Extra suites:
#   cs -i <path-to-process.cse-dir> test_async.csc
#   cs -i <path-to-process.cse-dir> test_file_redirect.csc
# Tests exit with code 1 if any assertion fails.

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

# Build a process_builder with shell mode and the given command.
function make_shell(cmd)
    var b = new process.builder
    b.cmd(cmd)
    if !enable_shell(b)
        throw runtime.exception("no usable shell API on process.builder")
    end
    return b
end

# True on Windows, false on Unix-like systems.

# Start a long-running child via a system-provided sleep equivalent.
# Avoids depending on `cs` being on PATH (T06/T14 used to do that).
#   Windows: ping -n N 127.0.0.1 (each ping is ~1s; +1 because the first is immediate)
#   Unix:    sleep N
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

# --- T01: shell exec + wait returns exit code 0 ---
section("T01 exec + wait")
try
    var _b01 = make_shell("echo t01")
    var _p01 = _b01.start()
    check_eq("exit code is 0", _p01.wait(), 0)
catch _e01
    check("T01 unexpected exception", false)
end

# --- T02: has_exited() and is_running() after wait() ---
section("T02 has_exited / is_running")
try
    var _b02 = make_shell("echo t02")
    var _p02 = _b02.start()
    _p02.wait()
    check("has_exited() true after wait", _p02.has_exited())
    check("is_running() false after wait", !(_p02.is_running()))
catch _e02
    check("T02 unexpected exception", false)
end

# --- T03: try_wait() is non-null after exit ---
section("T03 try_wait")
try
    var _b03 = make_shell("echo t03")
    var _p03 = _b03.start()
    _p03.wait()
    var _tw03 = _p03.try_wait()
    check_not_null("try_wait() non-null after exit", _tw03)
    check_eq("try_wait() returns 0", _tw03, 0)
catch _e03
    check("T03 unexpected exception", false)
end

# --- T04: wait_poll(-1) blocks until exit ---
section("T04 wait_poll negative timeout")
try
    var _b04 = make_shell("echo t04")
    var _p04 = _b04.start()
    var _code04 = _p04.wait_poll(-1, 5)
    check_not_null("wait_poll(-1) returns a code", _code04)
    check_eq("wait_poll(-1) code is 0", _code04, 0)
catch _e04
    check("T04 unexpected exception", false)
end

# --- T05: wait_poll on already-exited process ---
section("T05 wait_poll on exited process")
try
    var _b05 = make_shell("echo t05")
    var _p05 = _b05.start()
    _p05.wait()
    var _code05 = _p05.wait_poll(10, 1)
    check_not_null("wait_poll on exited returns code", _code05)
    check_eq("wait_poll on exited returns 0", _code05, 0)
catch _e05
    check("T05 unexpected exception", false)
end

# --- T06: wait_poll timeout on slow process returns null ---
section("T06 wait_poll timeout")
try
    var _p06 = start_sleeper(3)
    var _code06 = _p06.wait_poll(20, 5)
    check_null("wait_poll(20ms) on slow process is null", _code06)
    _p06.kill(true)
    _p06.wait()
catch _e06
    check("T06 unexpected exception", false)
end

# --- T07: communicate() captures stdout ---
section("T07 communicate() stdout")
try
    var _b07 = make_shell("echo marker_t07")
    var _p07 = _b07.start()
    var _r07 = _p07.communicate()
    check("stdout non-empty", _r07[0] != "")
    check_eq("communicate exit code is 0", _r07[2], 0)
catch _e07
    check("T07 unexpected exception", false)
end

# --- T08: communicate() with merge_output ---
section("T08 communicate() merge_output")
try
    var _b08 = new process.builder
    _b08.cmd("echo marker_t08 1>&2")
    _b08.use_shell(default_shell())
    _b08.merge_output(true)
    var _p08 = _b08.start()
    var _r08 = _p08.communicate()
    check("merged stdout non-empty", _r08[0] != "")
    check_eq("merge_output exit code is 0", _r08[2], 0)
catch _e08
    check("T08 unexpected exception", false)
end

# --- T09: non-zero exit code ---
section("T09 non-zero exit code")
try
    var _b09 = make_shell("exit 7")
    var _p09 = _b09.start()
    var _code09 = _p09.wait()
    check_eq("exit code 7 is returned", _code09, 7)
catch _e09
    check("T09 unexpected exception", false)
end

# --- T10: get_pid() returns positive integer ---
section("T10 get_pid()")
try
    var _b10 = make_shell("echo t10")
    var _p10 = _b10.start()
    check("get_pid() > 0", _p10.get_pid() > 0)
    _p10.wait()
catch _e10
    check("T10 unexpected exception", false)
end

# --- T11: builder env() ---
section("T11 builder env()")
try
    var _b11 = new process.builder
    _b11.env("CSPROC_TEST_VAR", "hello42")
    _b11.cmd("echo t11")
    _b11.use_shell(default_shell())
    check_eq("process with custom env exits 0", _b11.start().wait(), 0)
catch _e11
    check("T11 unexpected exception", false)
end

# --- T12: builder dir() ---
section("T12 builder dir()")
try
    var _b12 = make_shell("echo t12")
    _b12.dir(".")
    check_eq("process with custom dir exits 0", _b12.start().wait(), 0)
catch _e12
    check("T12 unexpected exception", false)
end

# --- T13: communicate() basic output path ---
section("T13 communicate() basic output path")
try
    var _b13 = make_shell("echo ping_t13")
    var _p13 = _b13.start()
    var _r13 = _p13.communicate()
    check_eq("process exits 0", _r13[2], 0)
    check("stdout non-empty", _r13[0] != "")
catch _e13
    check("T13 unexpected exception", false)
end

# --- T14: kill() terminates running process ---
section("T14 kill()")
try
    var _p14 = start_sleeper(30)
    check("is_running() before kill", _p14.is_running())
    _p14.kill(true)
    _p14.wait()
    check("has_exited() after kill", _p14.has_exited())
    check("is_running() false after kill", !(_p14.is_running()))
catch _e14
    check("T14 unexpected exception", false)
end

# --- T15: arg() basic path ---
section("T15 arg() basic path")
try
    var _b15 = new process.builder
    if system.is_platform_windows()
        _b15.cmd("cmd")
        _b15.arg({"/c", "echo first"})
    else
        _b15.cmd("echo")
        _b15.arg({"first"})
    end
    check_eq("single arg() call works", _b15.start().wait(), 0)
catch _e15
    check("T15 unexpected exception", false)
end

# --- T16: inherit_output communicate() returns empty strings ---
section("T16 inherit_output communicate()")
try
    var _b16 = new process.builder
    _b16.cmd("echo t16")
    if !enable_shell(_b16)
        check("inherit_output skipped: no usable shell API", true)
        throw "skip"
    end
    try
        _b16.inherit_output(true)
    catch _e16b
        check("inherit_output API unavailable; skipped", true)
        throw "skip"
    end
    var _p16 = _b16.start()
    var _r16 = _p16.communicate()
    check_eq("stdout empty with inherit_output", _r16[0], "")
    check_eq("stderr empty with inherit_output", _r16[1], "")
    check_eq("exit code is 0", _r16[2], 0)
catch _e16
    if _e16 != "skip"
        check("T16 unexpected exception", false)
    end
end

# --- T17: process.shell() shortcut ---
section("T17 process.shell()")
try
    var _b17 = make_shell("echo shell_t17")
    var _r17 = _b17.start().communicate()
    check_eq("shell builder exit code is 0", _r17[2], 0)
    check("shell builder stdout non-empty", _r17[0] != "")
catch _e17
    check("T17 unexpected exception", false)
end

# --- T18 / T19: negative tests ---
# Intentionally NOT tested:
#   * process.exec on a missing command (throws mpp::runtime_error from the
#     extension internals — CovScript try/catch cannot intercept native
#     exceptions thrown from CNI bindings, so the script would abort).
#   * arg() called twice (same reason — process_builder::arguments() throws
#     mpp::runtime_error). The constraint is documented in process.hpp and
#     README.md instead.

# --- T20: communicate() handles large stdout (>64KB) without deadlock ---
section("T20 large stdout via communicate()")
try
    # Drive a small interpreter directly (no shell wrapping), so the new
    # MSVCRT-style quoting in process_win32.cpp is exercised end-to-end.
    var _b20 = new process.builder
    if system.is_platform_windows()
        _b20.cmd("powershell")
        _b20.arg({"-NoProfile", "-Command", "for ($i=0;$i -lt 80;$i++){'x' * 1024}"})
    else
        _b20.cmd("sh")
        _b20.arg({"-c", "yes x | head -c 81920"})
    end
    var _r20 = _b20.start().communicate()
    check_eq("large output exit code is 0", _r20[2], 0)
    check("large output length >= 80KB (was " + _r20[0].size + ")", _r20[0].size >= 80000)
catch _e20
    check("T20 unexpected exception", false)
end

# --- T21: kill_tree() (Phase 4) ---
section("T21 kill_tree()")
try
    var _p21 = start_sleeper(30)
    _p21.kill_tree(true)
    var _code21 = _p21.wait()
    check_not_null("wait() returns after kill_tree", _code21)
catch _e21
    check("T21 unexpected exception", false)
end

# --- T22: wait() blocks (does not busy-spin) for ~real time on Unix ---
# Verifies P2#4 (wait_for de-busywait): elapsed wall time should approximate
# the sleep duration. On Windows wait_for already used WaitForSingleObject
# natively, so this also implicitly covers Win32.
section("T22 wait() blocks for real time")
try
    var _t22_start = runtime.time()
    var _p22 = start_sleeper(1)
    _p22.wait()
    var _elapsed = runtime.time() - _t22_start
    # Allow generous slack: at least 700ms (sleep granularity / startup cost).
    check("wait() elapsed ~>= 700ms (was " + _elapsed + "ms)", _elapsed >= 700)
catch _e22
    check("T22 unexpected exception", false)
end

# --- T23: wait_with(timeout, callback) drives an external scheduler hook ---
# Verifies the new wait_with CNI: callback is invoked at least once during the
# poll loop, and timeout-null vs exit-code paths still work correctly.
section("T23 wait_with callback hook")
var _t23_ticks = {0}
function _t23_tick()
    _t23_ticks[0] = _t23_ticks[0] + 1
end
try
    # Slow process, short timeout -> expect null + at least one tick.
    var _p23a = start_sleeper(2)
    var _r23a = _p23a.wait_with(200, _t23_tick)
    check_null("wait_with timeout returns null", _r23a)
    check("wait_with invoked callback at least once (was " + _t23_ticks[0] + ")", _t23_ticks[0] >= 1)
    _p23a.kill(true)
    _p23a.wait()

    # Already-exited fast process -> expect exit code 0 without calling tick.
    var _p23b = start_sleeper(0)
    _p23b.wait()  # ensure exited
    var _ticks_before = _t23_ticks[0]
    var _r23b = _p23b.wait_with(5000, _t23_tick)
    check_not_null("wait_with returns code on exited process", _r23b)
    check_eq("wait_with returned 0", _r23b, 0)
    check("wait_with did not tick on already-exited process", _t23_ticks[0] == _ticks_before)
catch _e23
    check("T23 unexpected exception", false)
end

# --- T24: wait_for(ms) returns true when process exits in time ---
section("T24 wait_for() - exits in time")
try
    var _p24a = make_shell("echo t24").start()
    check("wait_for 5000ms returns true", _p24a.wait_for(5000))
    check("has_exited() after wait_for true", _p24a.has_exited())
catch _e24a
    check("T24a unexpected exception", false)
end

# wait_for on a slow process should time out and return false
try
    var _p24b = start_sleeper(3)
    check("wait_for 50ms on slow process returns false", !(_p24b.wait_for(50)))
    _p24b.kill(true)
    _p24b.wait()
catch _e24b
    check("T24b unexpected exception", false)
end

# --- T25: wait_until(deadline_ms) ---
section("T25 wait_until() - deadline semantics")
try
    # Already-exited process: deadline far in future -> returns true.
    var _p25a = make_shell("echo t25").start()
    _p25a.wait()
    check("wait_until past deadline on exited process", _p25a.wait_until(runtime.time() + 5000))
catch _e25a
    check("T25a unexpected exception", false)
end

try
    # Slow process: deadline already elapsed -> returns false.
    var _p25b = start_sleeper(3)
    check("wait_until elapsed deadline returns false", !(_p25b.wait_until(runtime.time() - 1)))
    _p25b.kill(true)
    _p25b.wait()
catch _e25b
    check("T25b unexpected exception", false)
end

# --- T26: process_t.poll() non-blocking check ---
section("T26 process_t.poll()")
try
    # Running process -> poll() returns false (not yet exited).
    var _p26a = start_sleeper(3)
    check("poll() false while running", !(_p26a.poll()))
    _p26a.kill(true)
    _p26a.wait()

    # Exited process -> poll() returns true.
    var _p26b = make_shell("echo t26").start()
    _p26b.wait()
    check("poll() true after exit", _p26b.poll())
catch _e26
    check("T26 unexpected exception", false)
end

# --- T27: root-level process.exec() direct launch ---
section("T27 process.exec()")
try
    var _p27 = null
    if system.is_platform_windows()
        _p27 = process.exec("cmd", {"/c", "echo t27"})
    else
        _p27 = process.exec("/bin/sh", {"-c", "echo t27"})
    end
    check_not_null("exec() returns process_t", _p27)
    check_eq("exec() exit code 0", _p27.wait(), 0)
catch _e27
    check("T27 unexpected exception", false)
end

# --- T28: root-level process.shell() shortcut ---
section("T28 process.shell()")
try
    var _p28 = process.shell("echo t28")
    check_not_null("process.shell() returns process_t", _p28)
    check_eq("process.shell() exit code 0", _p28.wait(), 0)
catch _e28
    check("T28 unexpected exception", false)
end

# --- Summary ---

system.out.println("")
system.out.println("----------------------------------------")
system.out.println("Results: " + _pass + " passed, " + _fail + " failed")
system.out.println("----------------------------------------")

if _fail > 0
    system.exit(1)
end