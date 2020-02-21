#include <covscript/dll.hpp>
#include <covscript/cni.hpp>
#include <mozart++/process>

using process_t = std::shared_ptr<mpp::process>;
using builder_t = mpp::process_builder;

CNI_ROOT_NAMESPACE {
    CNI_V(exec, [](const std::string& cmd, const cs::array& args){
        std::vector<std::string> arr;
        for (auto &it:args)
            arr.emplace_back(it.const_val<std::string>());
        return std::make_shared<mpp::process>(mpp::process::exec(cmd, arr));
    })

    CNI_TYPE_EXT_V(builder_type, builder_t, builder, builder_t()) {
        CNI_V(cmd, [](const cs::var &b, const std::string &str){
            b.val<builder_t>().command(str);
            return b;
        })
        CNI_V(arg, [](const cs::var &b, const cs::array& args){
            std::vector<std::string> arr;
            for (auto &it:args)
                arr.emplace_back(it.const_val<std::string>());
            b.val<builder_t>().arguments(arr);
            return b;
        })
        CNI_V(dir, [](const cs::var &b, const std::string& str){
            b.val<builder_t>().directory(str);
            return b;
        })
        CNI_V(env, [](const cs::var &b, const std::string& key, const std::string& value){
            b.val<builder_t>().environment(key, value);
            return b;
        })
        CNI_V(merge_output, [](const cs::var &b, bool r){
            b.val<builder_t>().merge_outputs(r);
            return b;
        })
        CNI_V(start, [](builder_t &b){
            return std::make_shared<mpp::process>(b.start());
        })
    }

	CNI_NAMESPACE(process_type)
	{
		CNI_V(in, [](const process_t& p) {
			return cs::ostream(&p->in(), [](std::ostream *) {});
		})
		CNI_V(out, [](const process_t& p) {
			return cs::istream(&p->out(), [](std::istream *) {});
		})
		CNI_V(err, [](const process_t& p) {
			return cs::istream(&p->err(), [](std::istream *) {});
		})
		CNI_V(wait, [](const process_t &p) {
			return p->wait_for();
		})
		CNI_V(has_exited, [](const process_t &p) {
			return p->has_exited();
		})
		CNI_V(kill, [](const process_t &p, bool force) {
			return p->interrupt(force);
		})
	}
}

CNI_ENABLE_TYPE_EXT_V(builder_type, builder_t, process_builder)
CNI_ENABLE_TYPE_EXT_V(process_type, process_t, process)