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

	// Ensure a HANDLE has HANDLE_FLAG_INHERIT set so it can be passed to a
	// child process via STARTUPINFO.hStd* with bInheritHandles=TRUE.
	// If the handle is NULL/INVALID_HANDLE_VALUE this is a no-op (the child
	// will have no corresponding std handle, which is the best-effort fallback).
	static void ensure_handle_inheritable(HANDLE h, const char *name)
	{
		if (h == nullptr || h == INVALID_HANDLE_VALUE)
			return;
		DWORD flags = 0;
		if (!GetHandleInformation(h, &flags)) {
			auto le = GetLastError();
			std::string msg = "unable to query handle info on ";
			msg += name;
			msg += " (err=" + std::to_string(le) + ")";
			mpp::throw_ex<mpp::runtime_error>(msg);
		}
		if (!(flags & HANDLE_FLAG_INHERIT)) {
			if (!SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
				auto le = GetLastError();
				std::string msg = "unable to make ";
				msg += name;
				msg += " inheritable (err=" + std::to_string(le) + ")";
				mpp::throw_ex<mpp::runtime_error>(msg);
			}
		}
	}

	void create_process_impl(const process_startup &startup,
	                         process_info &info,
	                         fd_type *pstdin, fd_type *pstdout, fd_type *pstderr)
	{
		STARTUPINFO si;
		PROCESS_INFORMATION pi;

		ZeroMemory(&si, sizeof(si));
		si.cb = sizeof(si);
		si.dwFlags |= STARTF_USESTDHANDLES;

		// stdin: either inherit from parent terminal, redirect from a file handle,
		// or use a pipe.
		if (startup._inherit_stdin) {
			HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
			ensure_handle_inheritable(h, "stdin");
			si.hStdInput = h;
		}
		else if (startup._stdin.redirected()) {
			// Redirected: PIPE_READ == PIPE_WRITE == the file handle.
			// Ensure it is inheritable so the child can read from it.
			DWORD in_flags = 0;
			if (!GetHandleInformation(pstdin[PIPE_READ], &in_flags)) {
				auto le = GetLastError();
				std::string msg = "unable to query handle info on stdin redirect";
				msg += " (err=" + std::to_string(le);
				msg += ", h=" + std::to_string(reinterpret_cast<intptr_t>(pstdin[PIPE_READ])) + ")";
				mpp::throw_ex<mpp::runtime_error>(msg);
			}
			if (!(in_flags & HANDLE_FLAG_INHERIT)) {
				if (!SetHandleInformation(pstdin[PIPE_READ], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
					auto le = GetLastError();
					std::string msg = "unable to set handle info on stdin redirect";
					msg += " (err=" + std::to_string(le);
					msg += ", h=" + std::to_string(reinterpret_cast<intptr_t>(pstdin[PIPE_READ])) + ")";
					mpp::throw_ex<mpp::runtime_error>(msg);
				}
			}
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

		// stdout: inherit from parent terminal, redirect to a file handle, or use a pipe.
		if (startup._inherit_stdout) {
			HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
			ensure_handle_inheritable(h, "stdout");
			si.hStdOutput = h;
		}
		else if (startup._stdout.redirected()) {
			// Redirected: PIPE_WRITE == the file handle.
			// Ensure it is inheritable so the child can write to it.
			DWORD out_flags = 0;
			if (!GetHandleInformation(pstdout[PIPE_WRITE], &out_flags)) {
				auto le = GetLastError();
				std::string msg = "unable to query handle info on stdout redirect";
				msg += " (err=" + std::to_string(le);
				msg += ", h=" + std::to_string(reinterpret_cast<intptr_t>(pstdout[PIPE_WRITE])) + ")";
				mpp::throw_ex<mpp::runtime_error>(msg);
			}
			if (!(out_flags & HANDLE_FLAG_INHERIT)) {
				if (!SetHandleInformation(pstdout[PIPE_WRITE], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
					auto le = GetLastError();
					std::string msg = "unable to set handle info on stdout redirect";
					msg += " (err=" + std::to_string(le);
					msg += ", h=" + std::to_string(reinterpret_cast<intptr_t>(pstdout[PIPE_WRITE])) + ")";
					mpp::throw_ex<mpp::runtime_error>(msg);
				}
			}
			si.hStdOutput = pstdout[PIPE_WRITE];
		}
		else {
			// Pipe: the write end goes to the child via StdOutput; remove
			// inherit from the read end so the child does not unnecessarily
			// inherit it (matching stdin's treatment of the parent end).
			if (!SetHandleInformation(pstdout[PIPE_READ], HANDLE_FLAG_INHERIT, 0)) {
				mpp::throw_ex<mpp::runtime_error>("unable to set handle information on stdout");
			}
			si.hStdOutput = pstdout[PIPE_WRITE];
		}

		// stderr: merge into stdout, inherit from parent, redirect to a file, or use its own pipe
		if (startup._merge_outputs) {
			// merge: child stderr → same destination as child stdout
			si.hStdError = si.hStdOutput;
		}
		else if (startup._inherit_stderr) {
			HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
			ensure_handle_inheritable(h, "stderr");
			si.hStdError = h;
		}
		else if (startup._stderr.redirected()) {
			// Redirected: PIPE_WRITE == the file handle.
			// Ensure it is inheritable so the child can write to it.
			DWORD err_flags = 0;
			if (!GetHandleInformation(pstderr[PIPE_WRITE], &err_flags)) {
				auto le = GetLastError();
				std::string msg = "unable to query handle info on stderr redirect";
				msg += " (err=" + std::to_string(le);
				msg += ", h=" + std::to_string(reinterpret_cast<intptr_t>(pstderr[PIPE_WRITE])) + ")";
				mpp::throw_ex<mpp::runtime_error>(msg);
			}
			if (!(err_flags & HANDLE_FLAG_INHERIT)) {
				if (!SetHandleInformation(pstderr[PIPE_WRITE], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
					auto le = GetLastError();
					std::string msg = "unable to set handle info on stderr redirect";
					msg += " (err=" + std::to_string(le);
					msg += ", h=" + std::to_string(reinterpret_cast<intptr_t>(pstderr[PIPE_WRITE])) + ")";
					mpp::throw_ex<mpp::runtime_error>(msg);
				}
			}
			si.hStdError = pstderr[PIPE_WRITE];
		}
		else {
			// Pipe: the write end goes to the child via StdError; remove
			// inherit from the read end so the child does not unnecessarily
			// inherit it (matching stdin's treatment of the parent end).
			if (!SetHandleInformation(pstderr[PIPE_READ], HANDLE_FLAG_INHERIT, 0)) {
				mpp::throw_ex<mpp::runtime_error>("unable to set handle information on stderr");
			}
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
			}
			else {
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
					}
					else if (s[i] == '"') {
						// Backslashes preceding a quote: double them, then
						// escape the quote itself.
						ss << std::string(backslashes * 2 + 1, '\\') << '"';
						++i;
					}
					else {
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
						else if (eq == 0) {
							// Windows hidden variable: =X:=value (current dir per drive)
							auto eq2 = entry.find('=', 1);
							if (eq2 != std::string::npos && eq2 > 1)
								effective.emplace(entry.substr(0, eq2), entry.substr(eq2 + 1));
						}
						p += static_cast<ptrdiff_t>(entry.size()) + 1;
					}
					FreeEnvironmentStrings(parent_env);
				}
				for (const auto &e : startup._env)
					effective[e.first] = e.second;
			}
			else {
				// inherit_env=false: only _env (empty env block if _env is also empty).
				effective = startup._env;
			}

			// Windows requires the environment block to be sorted
			// alphabetically by variable name (case-insensitive).
			// Use CompareStringA with LOCALE_INVARIANT so that sort
			// order is independent of the thread locale.  Also filter
			// out entries with empty keys — they would produce ambiguous
			// "=value\0" entries indistinguishable from Windows hidden
			// drive variables.
			std::vector<std::pair<std::string, std::string>> sorted_env;
			sorted_env.reserve(effective.size());
			for (const auto &e : effective) {
				if (!e.first.empty())
					sorted_env.emplace_back(e);
			}
			std::sort(sorted_env.begin(), sorted_env.end(),
			[](const auto &a, const auto &b) {
				return CompareStringA(
				           LOCALE_INVARIANT,
				           NORM_IGNORECASE,
				           a.first.c_str(), -1,
				           b.first.c_str(), -1) == CSTR_LESS_THAN;
			});

			const size_t max_object_size = static_cast<size_t>(std::numeric_limits<std::ptrdiff_t>::max());
			// Non-empty blocks need one extra trailing NUL; empty blocks are just "\0\0".
			size_t env_size = sorted_env.empty() ? 2 : 1;
			for (const auto &e : sorted_env) {
				const size_t key_size = e.first.length();
				const size_t value_size = e.second.length();
				// need 2 more, which is the '=' and variable terminator '\0'
				// Guard against overflow in both the per-entry addition and
				// the accumulated total before performing any arithmetic.
				if (key_size > max_object_size - value_size - 2 ||
				        env_size > max_object_size - key_size - value_size - 2) {
					mpp::throw_ex<mpp::runtime_error>("environment block is too large");
				}
				env_size += key_size + value_size + 2;
			}

			envs_owner = std::make_unique<char[]>(env_size);
			envs = envs_owner.get();
			char *p = envs;

			for (const auto &e : sorted_env) {
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
			if (sorted_env.empty())
				*p++ = '\0'; // empty block needs a second NUL

			// ensure envs are copied correctly
			if (p != envs + env_size) {
				mpp::throw_ex<mpp::runtime_error>("unable to copy environment variables");
			}
		}

		// Only suppress the console window when not inheriting the parent's terminal.
		// If the caller uses any inherit flag the child should be visible in the
		// same console window as the parent.
		DWORD creation_flags = (startup._inherit_stdin || startup._inherit_stdout
		                        || startup._inherit_stderr) ? 0 : CREATE_NO_WINDOW;

		// CreateProcess may modify lpCommandLine; provide a writable buffer.
		std::vector<char> cmd_buf(command.begin(), command.end());
		cmd_buf.push_back('\0');

		if (!CreateProcess(nullptr, cmd_buf.data(),
		                   nullptr, nullptr, true, creation_flags, envs,
		                   startup._cwd.c_str(), &si, &pi)) {
			auto last_error = GetLastError();
			std::string msg = "unable to fork subprocess";
			msg += " (command: ";
			msg += command;
			msg += ", error code: ";
			msg += std::to_string(last_error);
			msg += ")";
			mpp::throw_ex<mpp::runtime_error>(msg);
		}
		// Close child-side ends that the parent doesn't need.
		// Redirect targets are owned by the caller (file_t); skip them.
		if (!startup._inherit_stdin && !startup._stdin.redirected())
			mpp_impl::close_fd(pstdin[PIPE_READ]);
		if (!startup._inherit_stdout && !startup._stdout.redirected())
			mpp_impl::close_fd(pstdout[PIPE_WRITE]);
		if (!startup._inherit_stderr && !startup._merge_outputs
		        && !startup._stderr.redirected())
			mpp_impl::close_fd(pstderr[PIPE_WRITE]);

		info._pid = pi.hProcess;
		info._tid = pi.hThread;
		// Record process creation time for diagnostics / future use.
		// On Windows, process identity is verified via the process handle
		// (WaitForSingleObject), which is more reliable than PID-based
		// checks, so _start_time is not used for PID-reuse detection here
		// (unlike the Unix implementation).
		{
			FILETIME ct, et, kt, ut;
			if (GetProcessTimes(pi.hProcess, &ct, &et, &kt, &ut))
				info._start_time = (static_cast<uint64_t>(ct.dwHighDateTime) << 32) | ct.dwLowDateTime;
		}
		// Only store pipe handles that we own.  Redirect targets belong to
		// the caller's file_t and inherited handles belong to the OS.
		info._stdin  = (startup._inherit_stdin  || startup._stdin.redirected())
		               ? FD_INVALID : pstdin[PIPE_WRITE];
		info._stdout = (startup._inherit_stdout || startup._stdout.redirected())
		               ? FD_INVALID : pstdout[PIPE_READ];
		info._stderr = (startup._merge_outputs || startup._inherit_stderr
		                || startup._stderr.redirected())
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
