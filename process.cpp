#include <covscript/dll.hpp>
#include <covscript/cni.hpp>
#include <process.hpp>

using covscript_process::process_t;
using covscript_process::process_builder;

CNI_ROOT_NAMESPACE {
	CNI_TYPE_EXT(builder, process_builder, process_builder())
	{
		CNI_V(redirect_stdin, &process_builder::redirect_stdin)
		CNI_V(redirect_stdout, &process_builder::redirect_stdout)
		CNI_V(redirect_stderr, &process_builder::redirect_stderr)
		CNI_V(start, &process_builder::start)
	}

	CNI_NAMESPACE(process)
	{
		CNI_V(in, &process_t::get_cs_stdin)
		CNI_V(out, &process_t::get_cs_stdout)
		CNI_V(err, &process_t::get_cs_stderr)
		CNI_V(wait, &process_t::wait_for_exit)
		CNI_V(exit_code, &process_t::get_exit_code)
		CNI_V(has_exited, &process_t::has_exited)
	}
}

CNI_ENABLE_TYPE_EXT_V(process, process_t, process)
CNI_ENABLE_TYPE_EXT_V(builder, process_builder, process::builder)