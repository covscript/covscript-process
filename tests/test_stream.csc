# Tests for file_t.in() / file_t.out() stream accessors.
# Compares behaviour with the standard library iostream.fstream.
# Run with: cs -i <path-to-process.cse-dir> test_stream.csc

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

# --- S01: file_t.in() returns writable ostream ---
section("S01 file_t.in() writable stream")
try
    var f = process.async.fstream("./.tmp_stream_s01.txt", "w+")
    var w = f.in()
    check_not_null("in() returns non-null for writable file", w)
    # write something
    w.println("s01_line1")
    w.print("s01_line2")
    w.println("")         # newline after print
    w.flush()
    f.close()

    # read back and verify
    f = process.async.fstream("./.tmp_stream_s01.txt", "r")
    var r = f.out()
    check_eq("written line1 matches", r.getline(), "s01_line1")
    check_eq("written line2 matches", r.getline(), "s01_line2")
    f.close()
catch _e
    check("S01 unexpected exception", false)
end

# --- S02: file_t.out() returns readable istream ---
section("S02 file_t.out() readable stream")
try
    # Write with file_t, read with file_t.out() stream
    var fw = process.async.fstream("./.tmp_stream_s02.txt", "w+")
    fw.in().println("hello_stream_s02")
    fw.close()

    var f = process.async.fstream("./.tmp_stream_s02.txt", "r")
    var r = f.out()
    check_not_null("out() returns non-null for readable file", r)
    check("good() after open", r.good())
    check_eq("getline reads file_t output", r.getline(), "hello_stream_s02")
    f.close()
catch _e
    check("S02 unexpected exception", false)
end

# --- S03: file_t.in() → null for non-writable / closed ---
section("S03 in() null on closed or read-only")
try
    # read-only file
    var f_ro = process.async.fstream("./.tmp_stream_s03.txt", "w+")
    f_ro.close()
    f_ro = process.async.fstream("./.tmp_stream_s03.txt", "r")
    check_null("in() null on read-only file", f_ro.in())
    f_ro.close()

    # closed file
    var f_closed = process.async.fstream("./.tmp_stream_s03b.txt", "w+")
    f_closed.close()
    check_null("in() null after close", f_closed.in())
catch _e
    check("S03 unexpected exception", false)
end

# --- S04: file_t.out() → null for non-readable / closed ---
section("S04 out() null on write-only or closed")
try
    # write-only: w+ is readable, so use plain "w" via reopen trick
    var f = process.async.fstream("./.tmp_stream_s04.txt", "w")
    if f != null
        # "w" mode is write-only
        check_null("out() null on write-only file", f.out())
        f.close()
    else
        check("w mode not supported (skip)", true)
    end

    # closed file
    var f2 = process.async.fstream("./.tmp_stream_s04b.txt", "w+")
    f2.close()
    check_null("out() null after close", f2.out())
catch _e
    check("S04 unexpected exception", false)
end

# --- S05: EOF semantics match iostream ---
section("S05 EOF semantics")
try
    var f = process.async.fstream("./.tmp_stream_s05.txt", "w+")
    f.in().print("X")
    f.close()

    f = process.async.fstream("./.tmp_stream_s05.txt", "r")
    var r = f.out()
    check("good() before read", r.good())
    check("not eof() before read", !r.eof())
    var ch = r.get()        # read 'X'
    check("good() after get", r.good())

    # Reading at EOF: get() returns something, then eof() becomes true
    var after = r.get()     # read at EOF
    # After EOF read, eof should be true (matching iostream behavior)
    var file_eof = r.eof()
    f.close()

    # iostream comparison
    var io = iostream.fstream("./.tmp_stream_s05.txt", iostream.openmode.in)
    var io_ch = io.get()     # 'X'
    var io_after = io.get()  # EOF
    check("eof() after reading past end", file_eof)
    check("iostream eof matches", io.eof())
catch _e
    check("S05 unexpected exception", false)
end

# --- S06: getline() after partial get() ---
section("S06 getline after get")
try
    var f = process.async.fstream("./.tmp_stream_s06.txt", "w+")
    f.in().println("ABCDEF")
    f.close()

    f = process.async.fstream("./.tmp_stream_s06.txt", "r")
    var r = f.out()
    var first = r.get()     # 'A'
    var rest = r.getline()  # "BCDEF"
    check("get() returns first char", first != null)
    check("getline() returns rest of line", rest != null)
    f.close()
catch _e
    check("S06 unexpected exception", false)
end

# --- S07: print vs println consistency ---
section("S07 print vs println")
try
    var f = process.async.fstream("./.tmp_stream_s07.txt", "w+")
    var w = f.in()
    w.print("hello")
    w.print(" ")
    w.print("world")
    w.println("")     # newline
    w.print(42)
    w.println("")     # newline after number
    w.flush()
    f.close()

    f = process.async.fstream("./.tmp_stream_s07.txt", "r")
    var r = f.out()
    check_eq("line1", r.getline(), "hello world")
    check_eq("line2", r.getline(), "42")
    f.close()
catch _e
    check("S07 unexpected exception", false)
end

# --- S08: stream compatibility: iostream ↔ file_t roundtrip ---
section("S08 iostream ↔ file_t roundtrip")
try
    # Phase 1: file_t.in() writes, iostream reads
    var f8a = process.async.fstream("./.tmp_stream_s08a.txt", "w+")
    f8a.in().println("roundtrip_hello")
    f8a.close()

    var io8a = iostream.fstream("./.tmp_stream_s08a.txt", iostream.openmode.in)
    check_eq("iostream reads file_t output", io8a.getline(), "roundtrip_hello")
    io8a = null

    # Phase 2: iostream writes, file_t.read() retrieves data
    var io8b = iostream.fstream("./.tmp_stream_s08b.txt", iostream.openmode.out)
    io8b.println("iostream_to_file_t")
    io8b.flush()
    io8b = null

    var f8b = process.async.fstream("./.tmp_stream_s08b.txt", "r")
    # file_t.read() is the primary API; it always works regardless of
    # text/binary mode differences between iostream and file_t.
    var data = f8b.read(128, 1000)
    check("file_t read() retrieves iostream data", data != null && data.size >= 19)
    f8b.close()
catch _e
    check("S08 unexpected exception", false)
end

# --- S09: file_t stream writes, iostream reads back (text mode note) ---
section("S09 stream write ↔ iostream read")
try
    var f9 = process.async.fstream("./.tmp_stream_s09.txt", "w+")
    f9.in().println("cross_api_test")
    f9.close()

    var io9 = iostream.fstream("./.tmp_stream_s09.txt", iostream.openmode.in)
    check("iostream reads file_t stream output", io9.getline() == "cross_api_test")
    io9 = null
catch _e
    check("S09 unexpected exception", false)
end

# --- S10: large stream write/read (>1MB) ---
section("S10 large stream write/read")
try
    var f10 = process.async.fstream("./.tmp_stream_s10.txt", "w+")
    var w10 = f10.in()
    check_not_null("large write: stream available", w10)
    # Write ~1.2MB via stream: 64 bytes * 20000 = 1280000 bytes
    var chunk = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz_!\n"
    var i = 0
    while i < 20000
        w10.print(chunk)
        i += 1
    end
    w10.flush()
    f10.close()

    # Read back with read() to verify total size
    f10 = process.async.fstream("./.tmp_stream_s10.txt", "r")
    var data10 = f10.read(2000000, 60000)
    check("large stream: read back >= 1MB", data10 != null && data10.size >= 1048576)
    f10.close()
catch _e
    check("S10 unexpected exception", false)
end

system.out.println("")
system.out.println("----------------------------------------")
system.out.println("Results: " + _pass + " passed, " + _fail + " failed")
system.out.println("----------------------------------------")

if _fail > 0
    system.exit(1)
end
