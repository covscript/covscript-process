/**
 * Mozart++ Template Library — forked from
 *   Chengdu Covariant Technologies Co., LTD. (2020-2021)
 *   https://covariant.cn/
 *   https://github.com/chengdu-zhirui/
 *
 * Licensed under Apache 2.0
 *
 * Copyright (C) 2017-2026 Michael Lee(李登淳)
 *
 * Email:   mikecovlee@163.com
 * Github:  https://github.com/mikecovlee
 * Website: http://covscript.org.cn
 */
#include <mozart++/core>

#ifdef MOZART_PLATFORM_WIN32

#include <mozart++/process>

#include <Windows.h>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>

namespace mpp_impl {
	void create_process_impl(const process_startup &startup,
	                         process_info &info,
	                         fd_type *pstdin, fd_type *pstdout, fd_type *pstderr)
	{
		STARTUPINFO si;
		PROCESS_INFORMATION pi;

		ZeroMemory(&si, sizeof(si));
		si.cb = sizeof(si);
		si.dwFlags |= STARTF_USESTDHANDLES;

		SECURITY_ATTRIBUTES sa;
		sa.nLength = sizeof(SECURITY_ATTRIBUTES);
		sa.bInheritHandle = true;
		sa.lpSecurityDescriptor = nullptr;

		// stdin: either inherit from parent terminal, redirect from a file handle,
		// or use a pipe.
		if (startup.inherit_stdin) {
			si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		}
		else if (startup._stdin.redirected()) {
			// Redirected: PIPE_READ == PIPE_WRITE == the file handle.
			// Ensure it is inheritable so the child can read from it.
			SetHandleInformation(pstdin[PIPE_READ], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
			si.hStdInput = pstdin[PIPE_READ];
		}
		else {
			// Pipe: the read end goes to the child; remove inherit from the write end
			// (parent retains it, child must not inherit it).
			if (!SetHandleInformation(pstdin[PIPE_WRITE], HANDLE_FLAG_INHERIT, 0)) {
				mpp::throw_ex<mpp::runtime_error>("unable to set handle information on stdin");
			}
			si.hStdInput = pstdin[PIPE_READ];
		}

		// stdout: either inherit from parent terminal or use our pipe
		si.hStdOutput = startup.inherit_stdout ? GetStdHandle(STD_OUTPUT_HANDLE) : pstdout[PIPE_WRITE];

		// stderr: merge into stdout, inherit from parent, or use its own pipe
		if (startup.merge_outputs || startup.inherit_stdout) {
			// merge: child stderr → same destination as child stdout
			si.hStdError = startup.inherit_stdout ? GetStdHandle(STD_OUTPUT_HANDLE) : pstdout[PIPE_WRITE];
		}
		else if (startup.inherit_stderr) {
			si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
		}
		else {
			si.hStdError = pstderr[PIPE_WRITE];
		}

		ZeroMemory(&pi, sizeof(pi));

		// Build the command line according to the MSVCRT command-line parsing
		// rules used by CommandLineToArgvW / the C runtime startup. The naive
		// algorithm (just escaping `"` as `\"`) is wrong when an argument
		// contains a literal backslash immediately before a double quote, or
		// trailing backslashes inside an argument that needs outer quoting,
		// because backslashes are treated as escape characters only when
		// followed by `"`. The correct algorithm is described in:
		//   - "Parsing C++ Command-Line Arguments" (Microsoft Docs)
		//   - Daniel Colascione, "Everyone quotes command line arguments the
		//     wrong way" (MSDN blog, 2011)
		//
		// Rules: count the run of consecutive backslashes preceding a quote
		// or the closing quote of the argument, and double them; backslashes
		// not preceding a quote are emitted as-is. Wrap the argument in outer
		// quotes only when it contains whitespace, a literal quote, or is
		// empty. Note: this produces the right argv for child processes that
		// use the standard MSVCRT parser; it does NOT escape cmd.exe shell
		// metacharacters (& | < > ^ %), which is intentional — those only
		// matter on the shell(true)/shell_cmd() path and are the user's
		// responsibility there.
		std::stringstream ss;
		for (const auto &s : startup._cmdline) {
			const bool need_quote = s.empty()
				|| s.find_first_of(" \t\n\v\"") != std::string::npos;

			if (!need_quote) {
				ss << s;
			} else {
				ss << '"';
				size_t i = 0;
				while (i < s.size()) {
					size_t backslashes = 0;
					while (i < s.size() && s[i] == '\\') {
						++backslashes;
						++i;
					}
					if (i == s.size()) {
						// Trailing backslashes before the closing quote: must
						// be doubled so the closing quote stays a delimiter.
						ss << std::string(backslashes * 2, '\\');
					} else if (s[i] == '"') {
						// Backslashes preceding a quote: double them, then
						// escape the quote itself.
						ss << std::string(backslashes * 2 + 1, '\\') << '"';
						++i;
					} else {
						// Backslashes not followed by a quote: emit verbatim.
						ss << std::string(backslashes, '\\') << s[i];
						++i;
					}
				}
				ss << '"';
			}
			ss << ' ';
		}

		std::string command = ss.str();

		char *envs = nullptr;
		std::unique_ptr<char[]> envs_owner;

		// CreateProcess with nullptr lpEnvironment inherits the parent's full env.
		// An explicit block (even "\0") replaces it entirely.
		if (!(startup._inherit_env && startup._env.empty())) {
			// Build effective env map.
			std::unordered_map<std::string, std::string> effective;
			if (startup._inherit_env) {
				// Merge: read parent env block, then apply _env overrides.
				LPCH parent_env = GetEnvironmentStrings();
				if (parent_env) {
					for (LPCH p = parent_env; *p; ) {
						std::string entry(p);
						auto eq = entry.find('=');
						if (eq != std::string::npos && eq > 0)
							effective.emplace(entry.substr(0, eq), entry.substr(eq + 1));
						p += static_cast<ptrdiff_t>(entry.size()) + 1;
					}
					FreeEnvironmentStrings(parent_env);
				}
				for (const auto &e : startup._env)
					effective[e.first] = e.second;
			} else {
				// inherit_env=false: only _env (empty env block if _env is also empty).
				effective = startup._env;
			}

			const size_t max_object_size = static_cast<size_t>(std::numeric_limits<std::ptrdiff_t>::max());
			// starting from 1, which is the block terminator '\0'
			size_t env_size = 1;
			for (const auto &e : effective) {
				const size_t key_size = e.first.length();
				const size_t value_size = e.second.length();
				// need 2 more, which is the '=' and variable terminator '\0'
				if (key_size > max_object_size - env_size - 2 ||
				        value_size > max_object_size - env_size - key_size - 2) {
					mpp::throw_ex<mpp::runtime_error>("environment block is too large");
				}
				env_size += key_size + value_size + 2;
			}

			envs_owner = std::make_unique<char[]>(env_size);
			envs = envs_owner.get();
			char *p = envs;

			for (const auto &e : effective) {
				const size_t key_size = e.first.length();
				const size_t value_size = e.second.length();
				std::memcpy(p, e.first.data(), key_size);
				p += key_size;
				*p++ = '=';
				std::memcpy(p, e.second.data(), value_size);
				p += value_size;
				*p++ = '\0'; // variable terminator
			}
			*p++ = '\0'; // block terminator

			// ensure envs are copied correctly
			if (p != envs + env_size) {
				mpp::throw_ex<mpp::runtime_error>("unable to copy environment variables");
			}
		}

		// Only suppress the console window when not inheriting the parent's terminal.
		// If the caller uses inherit_output(true) the child should be visible in the
		// same console window as the parent.
		DWORD creation_flags = startup.inherit_stdout ? 0 : CREATE_NO_WINDOW;

		if (!CreateProcess(nullptr, const_cast<char *>(command.c_str()),
		                   nullptr, nullptr, true, creation_flags, envs,
		                   startup._cwd.c_str(), &si, &pi)) {
			mpp::throw_ex<mpp::runtime_error>("unable to fork subprocess");
		}
		// Close child-side ends that the parent doesn't need.
		// Redirect targets are owned by the caller (file_t); skip them.
		if (!startup.inherit_stdin && !startup._stdin.redirected())
			mpp_impl::close_fd(pstdin[PIPE_READ]);
		if (!startup.inherit_stdout && !startup._stdout.redirected())
			mpp_impl::close_fd(pstdout[PIPE_WRITE]);
		if (!startup.inherit_stdout && !startup.inherit_stderr
		    && !startup.merge_outputs && !startup._stderr.redirected())
			mpp_impl::close_fd(pstderr[PIPE_WRITE]);

		info._pid = pi.hProcess;
		info._tid = pi.hThread;
		// Only store pipe handles that we own.  Redirect targets belong to
		// the caller's file_t and inherited handles belong to the OS.
		info._stdin  = (startup.inherit_stdin  || startup._stdin.redirected())
		               ? FD_INVALID : pstdin[PIPE_WRITE];
		info._stdout = (startup.inherit_stdout || startup._stdout.redirected())
		               ? FD_INVALID : pstdout[PIPE_READ];
		info._stderr = (startup.merge_outputs || startup.inherit_stdout
		                || startup.inherit_stderr || startup._stderr.redirected())
		               ? FD_INVALID : pstderr[PIPE_READ];
	}

	void close_process(process_info &info)
	{
		mpp_impl::close_fd(info._pid);
		mpp_impl::close_fd(info._tid);
		mpp_impl::close_fd(info._stdin);
		mpp_impl::close_fd(info._stdout);
		mpp_impl::close_fd(info._stderr);
	}
}

#endif
