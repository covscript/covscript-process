import process
var p = process.exec("/bin/echo", {"linux_exec"})
var r = p.communicate()
system.out.println("exit=" + r[2])
system.out.println("out=" + r[0])
