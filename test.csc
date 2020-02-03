import process
var p = process.start("cs -r -c " + context.cmd_args()[0], process.openmode.read)
var in = p.get_reader()
while in.good() && !in.eof()
    system.out.println(in.getline())
end