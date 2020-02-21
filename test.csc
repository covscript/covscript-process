import process
var p = process.exec("cs", {"-s"})
var ts = runtime.time()
p.in().println("runtime.info();runtime.delay(5000);system.exit(0)")
loop; until p.has_exited()
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
system.out.println("Process exited with code " + p.wait())