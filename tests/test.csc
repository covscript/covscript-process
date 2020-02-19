import process
var p = process.exec("cs", {"-s"})
p.in().println("runtime.info();system.exit(0)")
system.out.println(p.is_exited() ? "Process has exited" : "Process not exited yet")
var in = p.out()
while in.good() && !in.eof()
    system.out.println(in.getline())
end
system.out.println(p.is_exited() ? "Process has exited" : "Process not exited yet")
system.out.println("Process exited with code " + p.wait())