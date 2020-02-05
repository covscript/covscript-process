import process
var builder = new process.builder
builder.redirect_stdin(true)
builder.redirect_stdout(true)
var p = builder.start("cs", "-s")
p.in().println("runtime.info();system.exit(0)")
var in = p.out()
while in.good() && !in.eof()
    system.out.println(in.getline())
end
p.wait()