import process
var builder = new process.builder
builder.redirect_stdin(true)
builder.redirect_stdout(true)
var p = builder.start("cs", "-s")
p.in().println("runtime.info();system.exit(0)")
system.out.println(p.has_exited() ? "Process has exited" : "Process not exited yet")
var in = p.out()
while in.good() && !in.eof()
    system.out.println(in.getline())
end
system.out.println(p.has_exited() ? "Process has exited" : "Process not exited yet")
p.wait()
system.out.println("Process exited with code " + p.exit_code())