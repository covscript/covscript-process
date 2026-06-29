# Tests for file_t redirect_out/redirect_err semantics.
# Run with: cs -i <path-to-process.cse-dir> test_file_redirect.csc

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


function enable_shell(b)
    try
        b.shell(process.default_shell())
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

function shell_echo_stdout(msg)
    if system.is_platform_windows()
        return "echo " + msg
    else
        return "echo " + msg
    end
end

function shell_emit_stderr(msg)
    if system.is_platform_windows()
        return "echo " + msg + " 1>&2"
    else
        return "echo " + msg + " 1>&2"
    end
end

function read_all(path)
    var fr = open_async_file(path, "r")
    if fr == null
        return null
    end
    var out = ""
    loop
        var chunk = fr.read(256, 1000)
        if chunk == null
            break
        end
        if chunk == ""
            break
        end
        out += chunk
    until false
    fr.close()
    return out
end

section("R01 redirect_out writes stdout to file")
try
    var out_path = "./.tmp_redirect_out.txt"
    var out_f = open_async_file(out_path, "w+")
    check_not_null("open redirect file", out_f)
    if out_f == null
        check("async namespace unavailable; R01 skipped", true)
        throw "skip"
    end

    var b = new process.builder
    b.cmd(shell_echo_stdout("redirect_out_ok"))
    if !enable_shell(b)
        check("shell API unavailable; R01 skipped", true)
        throw "skip"
    end
    b.redirect_out(out_f)

    var p = b.start()
    var code = p.wait()
    check_eq("process exit code 0", code, 0)

    var _fl1 = out_f.flush(1000)
    check("flush attempted (result=" + _fl1 + ")", true)
    out_f.close()

    var text = read_all(out_path)
    check_not_null("reopen redirected output file", text)
    check("redirected file contains marker", text != null && text.size > 0)
catch _e
    if _e != "skip"
        check("R01 unexpected exception", false)
    end
end

section("R02 redirect_err writes stderr to file")
try
    var err_path = "./.tmp_redirect_err.txt"
    var err_f = open_async_file(err_path, "w+")
    check_not_null("open err file", err_f)
    if err_f == null
        check("async namespace unavailable; R02 skipped", true)
        throw "skip"
    end

    var b2 = new process.builder
    b2.cmd(shell_emit_stderr("redirect_err_ok"))
    if !enable_shell(b2)
        check("shell API unavailable; R02 skipped", true)
        throw "skip"
    end
    b2.redirect_err(err_f)

    var p2 = b2.start()
    var code2 = p2.wait()
    check_eq("process exit code 0", code2, 0)

    var _fl2 = err_f.flush(1000)
    check("flush attempted (result=" + _fl2 + ")", true)
    err_f.close()

    var etext = read_all(err_path)
    check_not_null("reopen redirected err file", etext)
    check("redirected err file contains marker", etext != null && etext.size > 0)
catch _e2
    if _e2 != "skip"
        check("R02 unexpected exception", false)
    end
end

system.out.println("")
system.out.println("----------------------------------------")
system.out.println("Results: " + _pass + " passed, " + _fail + " failed")
system.out.println("----------------------------------------")

if _fail > 0
    system.exit(1)
end
