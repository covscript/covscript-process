import process
var b = new process.builder
b.cmd("echo marker_t07")
if system.getenv("OS") == "Windows_NT"
	b.shell("cmd")
else
	b.shell("/bin/sh")
end
var p = b.start()
var r = p.communicate()
system.out.println(type(r))
system.out.println(type(r[0]))
system.out.println(r[0])
system.out.println(r[2])
