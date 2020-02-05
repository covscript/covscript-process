#include <covscript/dll.hpp>
#include <covscript/cni.hpp>
#include <process.hpp>

using covscript_process::process;
using covscript_process::process_builder;
using process_t = std::shared_ptr<process>;

CNI_ROOT_NAMESPACE {
    CNI_TYPE_EXT(builder, process_builder,  process_builder())
    {
        CNI_V(redirect_stdin,  &process_builder::redirect_stdin)
        CNI_V(redirect_stdout, &process_builder::redirect_stdout)
        CNI_V(redirect_stderr, &process_builder::redirect_stderr)
        CNI_V(start, [](process_builder &builder, const std::string &cmd, const std::string &arg){
            return std::make_shared<process>(builder.start(cmd, arg));
        })
    }

    CNI_NAMESPACE(process)
    {
        CNI_V(fucker, [](process_t &p) {
            p->fucker();
        })
        CNI_V(get_stdin, [](process_t &p){
            return cs::ostream(&p->get_stdin(), [](std::ostream *){});
        })
        CNI_V(get_stdout, [](process_t &p){
            return cs::istream(&p->get_stdout(), [](std::istream *){});
        })
        CNI_V(get_stderr, [](process_t &p){
            return cs::istream(&p->get_stderr(), [](std::istream *){});
        })
        CNI_V(wait_for_exit, [](process_t &p){
            p->wait_for_exit();
        })
    }
}

CNI_ENABLE_TYPE_EXT(builder, process_builder)
CNI_ENABLE_TYPE_EXT_V(process, process_t, process)