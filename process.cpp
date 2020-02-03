#include <covscript/dll.hpp>
#include <covscript/cni.hpp>
#include <cstdio>

#ifdef COVSCRIPT_PLATFORM_WIN32
#define popen_impl(...) ::_popen(__VA_ARGS__);
#define pclose_impl(...) ::_pclose(__VA_ARGS__);
#else
#define popen_impl(...) ::popen(__VA_ARGS__);
#define pclose_impl(...) ::pclose(__VA_ARGS__);
#endif

class classic_stream_reader final {
    static cs::var parse_value(const std::string &str) {
        if (str == "true")
            return true;
        if (str == "false")
            return false;
        try {
            return cs::parse_number(str);
        }
        catch (...) {
            return str;
        }
    }

    std::string read_until(const std::function<bool(int)> &terminator, bool ignore = true) {
        std::string buff;
        for (int ch = ::fgetc(s); good() && !eof() && !terminator(ch); ch = ::fgetc(s))
            buff.push_back(ch);
        if (ignore)
            for (int ch = ::fgetc(s); good() && !eof() && terminator(ch); ch = ::fgetc(s));
        return std::move(buff);
    }

    FILE *s = nullptr;
public:
    classic_stream_reader() = delete;

    explicit classic_stream_reader(FILE *fp) : s(fp) {
        if (s == nullptr)
            throw cs::lang_error("Creating StreamReader Failed.");
    }

    char get() {
        return ::fgetc(s);
    }

    bool good() {
        return !::ferror(s);
    }

    bool eof() {
        return ::feof(s);
    }

    std::string getline() {
        return read_until([](int ch) -> bool {
            return ch == '\r' || ch == '\n';
        });
    }

    cs::var input() {
        return parse_value(read_until([](int ch) -> bool { return std::isspace(ch); }));
    }
};

class classic_stream_writer final {
    FILE *s = nullptr;
public:
    classic_stream_writer() = delete;

    explicit classic_stream_writer(FILE *fp) : s(fp) {
        if (s == nullptr)
            throw cs::lang_error("Creating StreamWriter Failed.");
    }

    void put(char ch) {
        ::fputc(ch, s);
    }

    void flush() {
        ::fflush(s);
    }

    bool good() {
        return !::ferror(s);
    }

    void print(const cs::var &val) {
        ::fprintf(s, "%s", val.to_string().c_str());
    }

    void println(const cs::var &val) {
#ifdef COVSCRIPT_PLATFORM_WIN32
        ::fprintf(s, "%s\r\n", val.to_string().c_str());
#else
        ::fprintf(s, "%s\n", val.to_string().c_str());
#endif
    }
};

class process final {
public:
    enum class openmode {
        read, write, readwrite
    };
private:
    FILE *s = nullptr;
    openmode type;
public:
    process(const std::string &command, openmode mode) : type(mode) {
        switch (mode) {
            case openmode::read:
                s = popen_impl(command.c_str(), "r");
                break;
            case openmode::write:
                s = popen_impl(command.c_str(), "w");
                break;
            case openmode::readwrite:
                s = popen_impl(command.c_str(), "rw");
                break;
        }
        if (s == nullptr)
            throw cs::lang_error("Start process failed.");
    }

    process(const process &) = delete;

    ~process() {
        pclose_impl(s);
    }

    classic_stream_reader get_reader() {
        if (type == openmode::write)
            throw cs::lang_error("Can not read on write stream.");
        return classic_stream_reader(s);
    }

    classic_stream_writer get_writer() {
        if (type == openmode::read)
            throw cs::lang_error("Can not write on read stream.");
        return classic_stream_writer(s);
    }
};

using process_t = std::shared_ptr<process>;

CNI_ROOT_NAMESPACE {
    CNI_V(start, [](const std::string &command, process::openmode mode)
    {
        return cs::var::make<process_t>(std::make_shared<process>(command, mode));
    })

    CNI_NAMESPACE(process)
    {
        CNI_V(get_reader, [](process_t &p){
            return p->get_reader();
        })
        CNI_V(get_writer, [](process_t &p){
            return p->get_writer();
        })
    }

    CNI_NAMESPACE(stream_reader)
    {
        CNI_V(get,     &classic_stream_reader::get)
        CNI_V(good,    &classic_stream_reader::good)
        CNI_V(eof,     &classic_stream_reader::eof)
        CNI_V(getline, &classic_stream_reader::getline)
        CNI_V(input,   &classic_stream_reader::input)
    }

    CNI_NAMESPACE(stream_writer)
    {
        CNI_V(put,     &classic_stream_writer::put)
        CNI_V(flush,   &classic_stream_writer::flush)
        CNI_V(good,    &classic_stream_writer::good)
        CNI_V(print,   &classic_stream_writer::print)
        CNI_V(println, &classic_stream_writer::println)
    }
}

CNI_ENABLE_TYPE_EXT(process, process)
CNI_ENABLE_TYPE_EXT_V(stream_reader, classic_stream_reader, process::stream_reader)
CNI_ENABLE_TYPE_EXT_V(stream_writer, classic_stream_writer, process::stream_writer)