import process
var p = process.exec("cs", {"-s"})
var ts = runtime.time()
p.in().println("runtime.info();runtime.delay(5000);system.exit(0)")
loop
    var code = p.try_wait()
    if code != null
        system.out.println("Process exited with code " + code)
        break
    end
    runtime.delay(10) # cooperative polling, friendly to coroutine scheduler
until false
system.out.println("Time elapsed: " + (runtime.time() - ts))
system.out.println("")
var in = p.out()
while in.good()
    var ch = in.get()
    if in.eof()
        break
    end
    system.out.put(ch)
end
system.out.println("")
system.out.println(p.has_exited() ? "Process has exited" : "Process not exited yet")
system.out.println("wait_poll(0, 10) now returns: " + p.wait_poll(0, 10))