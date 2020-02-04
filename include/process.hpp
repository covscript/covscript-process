#pragma once

#include <streambuf>
#include <cstring>
#include <istream>
#include <ostream>
#include <memory>
#include <vector>
#include <cstdio>

#ifdef _WIN32

#include <Windows.h>

#else

#include <unistd.h>
#include <wait.h>

#endif

namespace covscript_process {
#ifdef _WIN32
    using fd_type = HANDLE;

    ssize_t read(fd_type handle, void *buf, size_t count) {
        DWORD dwRead;
        if (ReadFile(handle, buf, count, &dwRead, nullptr))
            return dwRead;
        else
            return 0;
    }

    ssize_t write(fd_type handle, const void *buf, size_t count) {
        DWORD dwWritten;
        if (WriteFile(handle, buf, count, &dwWritten, nullptr))
            return dwWritten;
        else
            return 0;
    }

#else
    using fd_type = int;
    using ::read;
    using ::write;
#endif

    class fdoutbuf : public std::streambuf {
    private:
        fd_type _fd;

    public:
        explicit fdoutbuf(fd_type _fd)
                : _fd(_fd) {
        }

    protected:
        int_type overflow(int_type c) override {
            if (c != EOF) {
                char z = c;
                if (write(_fd, &z, 1) != 1) {
                    return EOF;
                }
            }
            return c;
        }

        std::streamsize xsputn(const char *s,
                               std::streamsize num) override {
            return write(_fd, s, num);
        }
    };

    class fdostream : public std::ostream {
    private:
        fdoutbuf _buf;
    public:
        explicit fdostream(fd_type fd)
                : std::ostream(nullptr), _buf(fd) {
            rdbuf(&_buf);
        }
    };

    class fdinbuf : public std::streambuf {
    private:
        fd_type _fd;

    protected:
        /**
         * size of putback area
         */
        static constexpr size_t PUTBACK_SIZE = 4;

        /**
         * size of the data buffer
         */
        static constexpr size_t BUFFER_SIZE = 1024;

        char _buffer[BUFFER_SIZE + PUTBACK_SIZE]{0};

    public:
        explicit fdinbuf(fd_type _fd)
                : _fd(_fd) {
            setg(_buffer + PUTBACK_SIZE,     // beginning of putback area
                 _buffer + PUTBACK_SIZE,     // read position
                 _buffer + PUTBACK_SIZE);    // end position
        }

    protected:
        // insert new characters into the buffer
        int_type underflow() override {
            // is read position before end of buffer?
            if (gptr() < egptr()) {
                return traits_type::to_int_type(*gptr());
            }

            // handle putback area
            size_t backSize = gptr() - eback();
            if (backSize > PUTBACK_SIZE) {
                backSize = PUTBACK_SIZE;
            }

            // copy up to PUTBACK_SIZE characters previously read into
            // the putback area
            std::memmove(_buffer + (PUTBACK_SIZE - backSize),
                         gptr() - backSize,
                         backSize);

            // read at most BUFFER_SIZE new characters
            int num = read(_fd, _buffer + PUTBACK_SIZE, BUFFER_SIZE);
            if (num <= 0) {
                // it might be error happened somewhere or EOF encountered
                // we simply return EOF
                return EOF;
            }

            // reset buffer pointers
            setg(_buffer + (PUTBACK_SIZE - backSize),
                 _buffer + PUTBACK_SIZE,
                 _buffer + PUTBACK_SIZE + num);

            // return next character
            return traits_type::to_int_type(*gptr());
        }
    };

    class fdistream : public std::istream {
    private:
        fdinbuf _buf;
    public:
        explicit fdistream(fd_type fd)
                : std::istream(nullptr), _buf(fd) {
            rdbuf(&_buf);
        }
    };

    class process {
        friend class process_builder;

        struct process_info {
            bool redirect_stdin = false;
            bool redirect_stdout = false;
            bool redirect_stderr = false;
            std::string file, args;
        } psi;

        static constexpr int pipe_read = 0, pipe_write = 1;
        fd_type p_stdin[2];
        fd_type p_stdout[2];
        fd_type p_stderr[2];
        std::unique_ptr<fdostream> fd_stdin;
        std::unique_ptr<fdistream> fd_stdout;
        std::unique_ptr<fdistream> fd_stderr;

#ifdef _WIN32
        STARTUPINFO si;
        PROCESS_INFORMATION pi;

        explicit process(process_info __psi) : psi(std::move(__psi)) {
            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);
            if (psi.redirect_stdin || psi.redirect_stdout || psi.redirect_stderr)
                si.dwFlags |= STARTF_USESTDHANDLES;
            SECURITY_ATTRIBUTES sa;
            sa.nLength = sizeof(SECURITY_ATTRIBUTES);
            sa.bInheritHandle = true;
            sa.lpSecurityDescriptor = nullptr;
            if (psi.redirect_stdin) {
                if (!CreatePipe(&p_stdin[pipe_read], &p_stdin[pipe_write], &sa, 0))
                    throw std::runtime_error("Creating pipe of stdin failed.");
                si.hStdInput = p_stdin[pipe_read];
            }
            if (psi.redirect_stdout) {
                if (!CreatePipe(&p_stdout[pipe_read], &p_stdout[pipe_write], &sa, 0))
                    throw std::runtime_error("Creating pipe of stdout failed.");
                si.hStdOutput = p_stdout[pipe_write];
            }
            if (psi.redirect_stderr) {
                if (!CreatePipe(&p_stderr[pipe_read], &p_stderr[pipe_write], &sa, 0))
                    throw std::runtime_error("Creating pipe of stderr failed.");
                si.hStdError = p_stderr[pipe_write];
            }
            ZeroMemory(&pi, sizeof(pi));
            std::string command = psi.file + " " + psi.args;
            if (!CreateProcessA(nullptr, &command[0], nullptr, nullptr, false, 0,
                                nullptr, /* Current Directory in const char* */nullptr, &si, &pi))
                throw std::runtime_error("Creating subprocess failed.");
        }

#else
        pid_t pid;

        std::vector<std::string> split(const std::string &str)
        {
            std::vector<std::string> vec;
            std::string buff;
            for (auto &ch:str)
            {
                if (std::isspace(ch))
                {
                    if (!buff.empty()) {
                        vec.emplace_back(buff);
                        buff.clear();
                    }
                } else
                    buff.push_back(ch);
            }
            if (!buff.empty())
                vec.emplace_back(buff);
            return std::move(vec);
        }

        explicit process(process_info __psi) : psi(std::move(__psi)) {
            if (psi.redirect_stdin && pipe(p_stdin) != 0)
                throw std::runtime_error("Creating pipe of stdin failed.");
            if (psi.redirect_stdout && pipe(p_stdout) != 0)
                throw std::runtime_error("Creating pipe of stdout failed.");
            if (psi.redirect_stderr && pipe(p_stderr) != 0)
                throw std::runtime_error("Creating pipe of stderr failed.");
            pid = fork();
            if (pid == (pid_t)0)
            {
                if (psi.redirect_stdin) {
                    close(p_stdin[pipe_write]);
                    dup2(p_stdin[pipe_read], fileno(stdin));
                }
                if (psi.redirect_stdout) {
                    close(p_stdout[pipe_read]);
                    dup2(p_stdout[pipe_write], fileno(stdout));
                }
                if (psi.redirect_stderr) {
                    close(p_stderr[pipe_read]);
                    dup2(p_stderr[pipe_write], fileno(stderr));
                }
                std::vector<std::string> vec = split(psi.args);
                char *argv[vec.size()];
                for (std::size_t i = 0; i < vec.size(); ++i)
                    argv[i] = &vec[i][0];
                execv(psi.file.c_str(), argv);
                throw std::runtime_error("Execution of subprocess failed.");
            } else
                throw std::runtime_error("Creating subprocess failed.");
        }
#endif

    public:
        process() = delete;

        process(const process &) = delete;

        process(process &&) noexcept = default;

#ifdef _WIN32

        ~process() {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            if (psi.redirect_stdin) {
                CloseHandle(p_stdin[pipe_read]);
                CloseHandle(p_stdin[pipe_write]);
            }
            if (psi.redirect_stdout) {
                CloseHandle(p_stdout[pipe_read]);
                CloseHandle(p_stdout[pipe_write]);
            }
            if (psi.redirect_stderr) {
                CloseHandle(p_stderr[pipe_read]);
                CloseHandle(p_stderr[pipe_write]);
            }
        }

        void wait_for_exit() {
            WaitForSingleObject(pi.hProcess, INFINITE);
        }

#else
        ~process()
        {
            if (psi.redirect_stdin)
                close(p_stdin[pipe_write]);
            if (psi.redirect_stdout)
                close(p_stdout[pipe_read]);
            if (psi.redirect_stderr)
                close(p_stderr[pipe_read]);
        }

        void wait_for_exit() {
            int status;
            waitpid(pid, &status, 0);
        }
#endif

        std::ostream &get_stdin() {
            if (!psi.redirect_stdin)
                throw std::runtime_error("No redirection on stdin.");
            if (!fd_stdin)
                fd_stdin = std::make_unique<fdostream>(p_stdin[pipe_write]);
            return *fd_stdin;
        }

        std::istream &get_stdout() {
            if (!psi.redirect_stdout)
                throw std::runtime_error("No redirection on stdout.");
            if (!fd_stdout)
                fd_stdout = std::make_unique<fdistream>(p_stdout[pipe_read]);
            return *fd_stdout;
        }

        std::istream &get_stderr() {
            if (!psi.redirect_stderr)
                throw std::runtime_error("No redirection on stderr.");
            if (!fd_stderr)
                fd_stderr = std::make_unique<fdistream>(p_stderr[pipe_read]);
            return *fd_stderr;
        }
    };

    class process_builder {
        process::process_info psi;
    public:
        process_builder &redirect_stdin() {
            psi.redirect_stdin = true;
            return *this;
        }

        process_builder &redirect_stdout() {
            psi.redirect_stdout = true;
            return *this;
        }

        process_builder &redirect_stderr() {
            psi.redirect_stderr = true;
            return *this;
        }

        process start(const std::string &file, const std::string &args) {
            psi.file = file;
            psi.args = args;
            return process(psi);
        }
    };
}