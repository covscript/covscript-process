import process
system.out.println("type(process.builder)=" + type(process.builder))
var b = new process.builder
b.cmd("echo linux_smoke")
b.shell("/bin/sh")
var p = b.start()
var r = p.communicate()
system.out.println("exit=" + r[2])
system.out.println("out=" + r[0])
