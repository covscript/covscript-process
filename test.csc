import process
var builder = new process.builder
builder.redirect_stdin()
builder.redirect_stdout()
var p = builder.start("cs", "-s")
p.get_stdin().println("runtime.info();system.exit(0)")
var in = p.get_stdout()
while in.good() && !in.eof()
    system.out.println(in.getline())
end
p.wait_for_exit()