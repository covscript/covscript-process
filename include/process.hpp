#pragma once

#ifndef NO_COVSCRIPT
#include <covscript/core/core.hpp>
#endif

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

#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#endif

namespace covscript_process {
#ifndef NO_COVSCRIPT
	using runtime_exception = cs::lang_error;
	using critical_exception = cs::runtime_error;
#else
	using runtime_exception = std::logic_error;
	using critical_exception = std::runtime_error;
#endif

#ifdef _WIN32
	using fd_type = HANDLE;

	/**
	 * Implementation of unix style read/write on Win32 HANDLE
	 */
	ssize_t read(fd_type handle, void *buf, size_t count)
	{
		DWORD dwRead;
		if (ReadFile(handle, buf, count, &dwRead, nullptr))
			return dwRead;
		else
			return 0;
	}

	ssize_t write(fd_type handle, const void *buf, size_t count)
	{
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
			: _fd(_fd)
		{
		}

	protected:
		int_type overflow(int_type c) override
		{
			if (c != EOF) {
				char z = c;
				if (write(_fd, &z, 1) != 1) {
					return EOF;
				}
			}
			return c;
		}

		std::streamsize xsputn(const char *s,
		                       std::streamsize num) override
		{
			return write(_fd, s, num);
		}
	};

	class fdostream : public std::ostream {
	private:
		fdoutbuf _buf;

	public:
		explicit fdostream(fd_type fd)
			: std::ostream(nullptr), _buf(fd)
		{
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

		char _buffer[BUFFER_SIZE + PUTBACK_SIZE] {0};

	public:
		explicit fdinbuf(fd_type _fd)
			: _fd(_fd)
		{
			setg(_buffer + PUTBACK_SIZE,  // beginning of putback area
			     _buffer + PUTBACK_SIZE,  // read position
			     _buffer + PUTBACK_SIZE); // end position
		}

	protected:
		// insert new characters into the buffer
		int_type underflow() override
		{
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
			: std::istream(nullptr), _buf(fd)
		{
			rdbuf(&_buf);
		}
	};

	class process_t {
		friend class process_builder;

		struct process_info {
			std::string file, args, dir = ".";
			bool redirect_stdin = false;
			bool redirect_stdout = false;
			bool redirect_stderr = false;
		};

		class system_process {
			static constexpr int pipe_read = 0;
			static constexpr int pipe_write = 1;

			// Native Handles of PIPE
			fd_type p_stdin[2];
			fd_type p_stdout[2];
			fd_type p_stderr[2];

			// Wrapper for Native Handles
			std::unique_ptr<fdostream> fd_stdin;
			std::unique_ptr<fdistream> fd_stdout;
			std::unique_ptr<fdistream> fd_stderr;

			process_info m_psi;

			int exit_code = 0;

#ifdef _WIN32
			/**
			 * Win32 Process Implementation
			 */
			STARTUPINFO si;
			PROCESS_INFORMATION pi;

		public:
			explicit system_process(process_info psi) : m_psi(std::move(psi))
			{
				ZeroMemory(&si, sizeof(si));
				si.cb = sizeof(si);
				if (m_psi.redirect_stdin || m_psi.redirect_stdout || m_psi.redirect_stderr)
					si.dwFlags |= STARTF_USESTDHANDLES;
				SECURITY_ATTRIBUTES sa;
				sa.nLength = sizeof(SECURITY_ATTRIBUTES);
				sa.bInheritHandle = true;
				sa.lpSecurityDescriptor = nullptr;
				if (m_psi.redirect_stdin) {
					if (!CreatePipe(&p_stdin[pipe_read], &p_stdin[pipe_write], &sa, 0))
						throw critical_exception("Creating pipe of stdin failed.");
					if (!SetHandleInformation(p_stdin[pipe_write], HANDLE_FLAG_INHERIT, 0))
						throw critical_exception("Creating pipe of stdin failed.");
					si.hStdInput = p_stdin[pipe_read];
				}
				if (m_psi.redirect_stdout) {
					if (!CreatePipe(&p_stdout[pipe_read], &p_stdout[pipe_write], &sa, 0))
						throw critical_exception("Creating pipe of stdout failed.");
					si.hStdOutput = p_stdout[pipe_write];
				}
				if (m_psi.redirect_stderr) {
					if (!CreatePipe(&p_stderr[pipe_read], &p_stderr[pipe_write], &sa, 0))
						throw critical_exception("Creating pipe of stderr failed.");
					si.hStdError = p_stderr[pipe_write];
				}
				ZeroMemory(&pi, sizeof(pi));
				std::string command = m_psi.file + " " + m_psi.args;
				if (!CreateProcess(nullptr, const_cast<char *>(command.c_str()), nullptr, nullptr, true,
				                   CREATE_NO_WINDOW, nullptr,
				                   m_psi.dir.c_str(), &si, &pi))
					throw runtime_exception("Creating subprocess failed.");
				if (m_psi.redirect_stdin)
					CloseHandle(p_stdin[pipe_read]);
				if (m_psi.redirect_stdout)
					CloseHandle(p_stdout[pipe_write]);
				if (m_psi.redirect_stderr)
					CloseHandle(p_stderr[pipe_write]);
			}

			~system_process()
			{
				CloseHandle(pi.hProcess);
				CloseHandle(pi.hThread);
				if (m_psi.redirect_stdin) {
					CloseHandle(p_stdin[pipe_read]);
					CloseHandle(p_stdin[pipe_write]);
				}
				if (m_psi.redirect_stdout) {
					CloseHandle(p_stdout[pipe_read]);
					CloseHandle(p_stdout[pipe_write]);
				}
				if (m_psi.redirect_stderr) {
					CloseHandle(p_stderr[pipe_read]);
					CloseHandle(p_stderr[pipe_write]);
				}
			}

			bool has_exited()
			{
				return !WaitForSingleObject(pi.hProcess, 0);
			}

			int wait_for_exit()
			{
				WaitForSingleObject(pi.hProcess, INFINITE);
				DWORD ec;
				if (GetExitCodeProcess(pi.hProcess, &ec))
					return exit_code = ec;
				else
					return -1;
			}

#else
			/**
			 * Unix Process Implementation
			 */
			pid_t pid;

			std::vector<std::string> split(const std::string &str)
			{
				std::vector<std::string> vec;
				std::string buff;
				for (auto &ch : str) {
					if (std::isspace(ch)) {
						if (!buff.empty()) {
							vec.emplace_back(buff);
							buff.clear();
						}
					}
					else
						buff.push_back(ch);
				}
				if (!buff.empty())
					vec.emplace_back(buff);
				return std::move(vec);
			}

			void set_cloexec(int fd)
			{
				if (fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC) == -1)
					throw critical_exception("Error setting FD_CLOEXEC flag.");
			}

		public:
			explicit system_process(process_info psi) : m_psi(std::move(psi))
			{
				if (m_psi.redirect_stdin && pipe(p_stdin) != 0)
					throw critical_exception("Creating pipe of stdin failed.");
				if (m_psi.redirect_stdout && pipe(p_stdout) != 0)
					throw critical_exception("Creating pipe of stdout failed.");
				if (m_psi.redirect_stderr && pipe(p_stderr) != 0)
					throw critical_exception("Creating pipe of stderr failed.");
				pid = fork();
				if (pid == 0) {
					if (m_psi.redirect_stdin) {
						close(p_stdin[pipe_write]);
						dup2(p_stdin[pipe_read], fileno(stdin));
						close(p_stdin[pipe_read]);
					}
					if (m_psi.redirect_stdout) {
						close(p_stdout[pipe_read]);
						dup2(p_stdout[pipe_write], fileno(stdout));
						close(p_stdout[pipe_write]);
					}
					if (m_psi.redirect_stderr) {
						close(p_stderr[pipe_read]);
						dup2(p_stderr[pipe_write], fileno(stderr));
						close(p_stderr[pipe_write]);
					}
					std::vector<std::string> vec = split(m_psi.args);
					char *argv[vec.size() + 2];
					argv[0] = const_cast<char *>(m_psi.file.c_str());
					for (std::size_t i = 0; i < vec.size(); ++i)
						argv[i + 1] = const_cast<char *>(vec[i].c_str());
					argv[vec.size() + 1] = nullptr;
					if (chdir(m_psi.dir.c_str()) == -1)
						throw runtime_exception("Change workding dir failed.");
					execvp(m_psi.file.c_str(), argv);
					throw runtime_exception("Execution of subprocess failed.");
				}
				else if (pid < 0)
					throw runtime_exception("Creating subprocess failed.");
				if (m_psi.redirect_stdin) {
					close(p_stdin[pipe_read]);
					set_cloexec(p_stdin[pipe_write]);
				}
				if (m_psi.redirect_stdout) {
					close(p_stdout[pipe_write]);
					set_cloexec(p_stdout[pipe_read]);
				}
				if (m_psi.redirect_stderr) {
					close(p_stderr[pipe_write]);
					set_cloexec(p_stderr[pipe_read]);
				}
			}

			~system_process()
			{
				if (m_psi.redirect_stdin)
					close(p_stdin[pipe_write]);
				if (m_psi.redirect_stdout)
					close(p_stdout[pipe_read]);
				if (m_psi.redirect_stderr)
					close(p_stderr[pipe_read]);
			}

			bool has_exited()
			{
				return waitpid(pid, nullptr, WNOHANG);
			}

			int wait_for_exit()
			{
				int status;
				waitpid(pid, &status, 0);
				if (WIFEXITED(status))
					return exit_code = WEXITSTATUS(status);
				else
					return -1;
			}
#endif

			int get_exit_code()
			{
				if (!has_exited())
					throw runtime_exception("Process not exited yet.");
				return exit_code;
			}

			std::ostream &get_stdin()
			{
				if (!m_psi.redirect_stdin)
					throw runtime_exception("No redirection on stdin.");
				if (!fd_stdin)
					fd_stdin = std::make_unique<fdostream>(p_stdin[pipe_write]);
				return *fd_stdin;
			}

			std::istream &get_stdout()
			{
				if (!m_psi.redirect_stdout)
					throw runtime_exception("No redirection on stdout.");
				if (!fd_stdout)
					fd_stdout = std::make_unique<fdistream>(p_stdout[pipe_read]);
				return *fd_stdout;
			}

			std::istream &get_stderr()
			{
				if (!m_psi.redirect_stderr)
					throw runtime_exception("No redirection on stderr.");
				if (!fd_stderr)
					fd_stderr = std::make_unique<fdistream>(p_stderr[pipe_read]);
				return *fd_stderr;
			}
		};

		std::shared_ptr<system_process> process;

		explicit process_t(const process_info &psi) : process(std::make_shared<system_process>(psi)) {}

	public:
		process_t() = delete;

		std::ostream &get_stdin() const
		{
			return process->get_stdin();
		}

		std::istream &get_stdout() const
		{
			return process->get_stdout();
		}

		std::istream &get_stderr() const
		{
			return process->get_stderr();
		}

#ifndef NO_COVSCRIPT
		cs::ostream get_cs_stdin() const
		{
			return cs::ostream(&process->get_stdin(), [](std::ostream *) {});
		}

		cs::istream get_cs_stdout() const
		{
			return cs::istream(&process->get_stdout(), [](std::istream *) {});
		}

		cs::istream get_cs_stderr() const
		{
			return cs::istream(&process->get_stderr(), [](std::istream *) {});
		}
#endif

		bool has_exited() const
		{
			return process->has_exited();
		}

		int wait_for_exit() const
		{
			return process->wait_for_exit();
		}

		int get_exit_code() const
		{
			return process->get_exit_code();
		}
	};

	class process_builder {
		process_t::process_info psi;

	public:
		void redirect_stdin(bool val)
		{
			psi.redirect_stdin = val;
		}

		void redirect_stdout(bool val)
		{
			psi.redirect_stdout = true;
		}

		void redirect_stderr(bool val)
		{
			psi.redirect_stderr = true;
		}

		void working_dir(const std::string &path)
		{
			psi.dir = path;
		}

		process_t start(const std::string &file, const std::string &args)
		{
			psi.file = file;
			psi.args = args;
			return process_t(psi);
		}
	};
} // namespace covscript_process