# Tests for process operations running inside fibers.
# Run with: cs -i <path-to-process.cse-dir> test_fiber.csc
# These tests exercise the fiber-aware code paths in the CNI layer
# (begin_wait/poll_wait + cs::fiber::yield instead of blocking wait).

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

# --- Fiber result holders ---
var _fr01 = null  # F01 wait result
var _fr02_out = null  # F02 stdout
var _fr02_ec = null   # F02 exit code
var _fr03 = null  # F03 wait_poll result
var _fr04_running = false  # F04
var _fr04_killed = false   # F04
var _fr05_0 = null  # F05 results
var _fr05_1 = null
var _fr05_2 = null
var _fr06_poll = null  # F06
var _fr06_exited = null

# --- Fiber bodies ---
function f01_body()
    var p = process.shell("echo fiber_hello")
    _fr01 = p.wait()
end

function f02_body()
    var p = process.shell("echo fiber_comm")
    var r = p.communicate()
    _fr02_out = r[0]
    _fr02_ec = r[2]
end

function f03_body()
    var p = process.shell("echo fiber_poll")
    _fr03 = p.wait_poll(5000, 5)
end

function f04_body()
    var sleep_cmd = ""
    if system.is_platform_windows()
        sleep_cmd = "ping -n 31 127.0.0.1 >nul"
    else
        sleep_cmd = "sleep 30"
    end
    var p = process.shell(sleep_cmd)
    _fr04_running = p.is_running()
    p.kill(true)
    p.wait()
    _fr04_killed = p.has_exited()
end

function f05a_body()
    var p = process.shell("echo f1")
    _fr05_0 = p.wait()
end

function f05b_body()
    var p = process.shell("echo f2")
    _fr05_1 = p.wait()
end

function f05c_body()
    var p = process.shell("echo f3")
    _fr05_2 = p.wait()
end

function f06_body()
    var p = process.shell("echo fiber_poll_check")
    _fr06_poll = p.has_exited()
    p.wait()
    _fr06_exited = p.has_exited()
end

# Helper: resume a fiber until it finishes
function run_fiber(fib)
    while !fib.is_finished()
        fib.resume()
    end
end

# --- F01: process.wait() inside a fiber ---
section("F01 wait() in fiber")
try
    var fib = fiber.create(f01_body)
    run_fiber(fib)
    check_eq("wait() in fiber returns 0", _fr01, 0)
catch _e
    check("F01 unexpected exception", false)
end

# --- F02: process.communicate() inside a fiber ---
section("F02 communicate() in fiber")
try
    var fib = fiber.create(f02_body)
    run_fiber(fib)
    check_eq("communicate() in fiber exit code 0", _fr02_ec, 0)
    check("communicate() in fiber stdout non-empty", _fr02_out != "")
catch _e
    check("F02 unexpected exception", false)
end

# --- F03: process.wait_poll() inside a fiber ---
section("F03 wait_poll() in fiber")
try
    var fib = fiber.create(f03_body)
    run_fiber(fib)
    check_eq("wait_poll() in fiber returns 0", _fr03, 0)
catch _e
    check("F03 unexpected exception", false)
end

# --- F04: kill() + is_running() inside a fiber ---
section("F04 kill in fiber")
try
    var fib = fiber.create(f04_body)
    run_fiber(fib)
    check("is_running() in fiber before kill", _fr04_running)
    check("has_exited() in fiber after kill", _fr04_killed)
catch _e
    check("F04 unexpected exception", false)
end

# --- F05: multiple concurrent fibers with process ---
section("F05 concurrent fibers")
try
    var fibers = new array
    fibers.push_back(fiber.create(f05a_body))
    fibers.push_back(fiber.create(f05b_body))
    fibers.push_back(fiber.create(f05c_body))
    var all_done = false
    while !all_done
        all_done = true
        foreach f in fibers
            if !f.is_finished()
                all_done = false
                f.resume()
            end
        end
    end
    check_eq("fiber 1 exit code 0", _fr05_0, 0)
    check_eq("fiber 2 exit code 0", _fr05_1, 0)
    check_eq("fiber 3 exit code 0", _fr05_2, 0)
catch _e
    check("F05 unexpected exception", false)
end

# --- F06: process_t.has_exited() inside a fiber ---
section("F06 has_exited in fiber")
try
    var fib = fiber.create(f06_body)
    run_fiber(fib)
    check("has_exited() before wait in fiber", _fr06_poll == true || _fr06_poll == false)
    check("has_exited() true after wait in fiber", _fr06_exited)
catch _e
    check("F06 unexpected exception", false)
end

system.out.println("")
system.out.println("----------------------------------------")
system.out.println("Results: " + _pass + " passed, " + _fail + " failed")
system.out.println("----------------------------------------")

if _fail > 0
    system.exit(1)
end
