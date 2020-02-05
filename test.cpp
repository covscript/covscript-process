#define NO_COVSCRIPT

#include <process.hpp>
#include <iostream>

using namespace covscript_process;

int main()
{
	process_builder builder;
	builder.redirect_stdin(true);
	builder.redirect_stdout(true);
	builder.redirect_stderr(true);
	auto p = builder.start("cs", "-s");
	p.get_stdin() << "runtime.info(); system.exit(0)" << std::endl;
	while (p.get_stdout()) {
		std::string str;
		std::getline(p.get_stdout(), str);
		std::cout << str << std::endl;
	}
	p.wait_for_exit();
	return 0;
}