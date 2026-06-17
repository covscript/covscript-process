/**
 * Mozart++ Template Library
 * Licensed under Apache 2.0
 * Copyright (C) 2020-2021 Chengdu Covariant Technologies Co., LTD.
 * Website: https://covariant.cn/
 * Github:  https://github.com/chengdu-zhirui/
 */
#pragma once

#include <mozart++/core>
#include <mozart++/fdstream>
#include <optional>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <future>
#include <sstream>
#include <thread>
#include <vector>
#include <string>
#include <memory>
#include <cstdint>

namespace mpp_impl {
	using mpp::fd_type;
	using mpp::FD_INVALID;
	using mpp::close_fd;
	using mpp::close_pipe;
	using mpp::create_pipe;
	using mpp::PIPE_READ;
	using mpp::PIPE_WRITE;

	struct redirect_info {
		fd_type _target = FD_INVALID;

		bool redirected() const
		{
			return _target != FD_INVALID;
		}
	};

	struct process_startup {
		std::vector<std::string> _cmdline;
		std::optional<std::string> _shell_program;
		std::unordered_map<std::string, std::string> _env;
		// When true (default), the child inherits the parent's environment,
		// with _env entries applied as overrides.  When false only _env is used.
		bool _inherit_env = true;
		std::string _cwd = ".";
		redirect_info _stdin;
		redirect_info _stdout;
		redirect_info _stderr;
		bool merge_outputs = false;
		// When true the corresponding stream is inherited from the parent
		// process rather than being connected via a pipe.
		bool inherit_stdin = false;
		bool inherit_stdout = false;
		bool inherit_stderr = false;
		// When true, the command is wrapped in a shell (sh -c / cmd /c).
		bool shell_mode = false;
	};

	struct process_info {
		/**
		 * Unused on *nix systems.
		 */
		fd_type _tid = FD_INVALID;

		fd_type _pid = FD_INVALID;
		fd_type _stdin = FD_INVALID;
		fd_type _stdout = FD_INVALID;
		fd_type _stderr = FD_INVALID;
	};

	void create_process_impl(const process_startup &startup,
	                         process_info &info,
	                         fd_type *pstdin, fd_type *pstdout, fd_type *pstderr);

	bool redirect_or_pipe(const redirect_info &r, fd_type fds[2]);

	void create_process(const process_startup &startup, process_info &info);

	void close_process(process_info &info);

	int wait_for(const process_info &info);

	void terminate_process(const process_info &info, bool force);

	void terminate_process_tree(const process_info &info, bool force);

	bool process_exited(const process_info &info);

	/**
	 * Wait for the process to exit, but at most timeout_ms milliseconds.
	 * Returns true and sets exit_code if the process exited within the timeout;
	 * returns false (and leaves exit_code unchanged) if it timed out.
	 * poll_interval_ms controls the sleep between polls on platforms that need it
	 * (Unix); on Windows the OS wakes us natively, so this value is ignored.
	 */
	bool wait_timeout_ms(const process_info &info, int timeout_ms, int &exit_code,
	                     int poll_interval_ms = 5);

	/**
	 * Return the OS-level process ID (integer PID on *nix, dwProcessId on Win32).
	 */
	int get_pid(const process_info &info);
}

namespace mpp {
	using mpp_impl::redirect_info;
	using mpp_impl::process_info;
	using mpp_impl::process_startup;
	using mpp_impl::fd_type;

	class process {
		friend class process_builder;

	private:
		struct member_holder {
			process_info _info;
			fdostream _stdin;
			fdistream _stdout;
			fdistream _stderr;
			// Use optional to distinguish "not yet waited" from exit code 0 or negative.
			std::optional<int> _exit_code;
			// Set to true once we observe via OS poll that the process has exited,
			// so subsequent has_exited() calls skip the OS round-trip.
			bool _observed_exited = false;
			// Reader futures for the asynchronous communicate() split interface.
			// Populated by begin_communicate(); consumed by end_communicate().
			std::future<std::string> _out_future;
			std::future<std::string> _err_future;
			// Waiter future for the asynchronous wait split interface.
			// Populated by begin_wait(); consumed by collect_wait().
			std::future<int> _wait_future;

			explicit member_holder(const process_info &info)
				: _info(info), _stdin(_info._stdin),
				  _stdout(_info._stdout), _stderr(_info._stderr) {}

			~member_holder()
			{
				// Join any in-flight futures before closing file descriptors.
				if (_wait_future.valid()) _wait_future.wait();
				if (_out_future.valid()) _out_future.wait();
				if (_err_future.valid()) _err_future.wait();
				mpp_impl::close_process(_info);
			}
		};

		std::unique_ptr<member_holder> _this;

		explicit process(const process_info &info)
			: _this(std::make_unique<member_holder>(info)) {}

	public:
		process() = delete;

		process(const process &) = delete;

		process(process &&) = default;

		process &operator=(process &&) = delete;

		process &operator=(const process &) = delete;

	public:
		~process() = default;

		std::ostream &in()
		{
			return _this->_stdin;
		}

		std::istream &out()
		{
			return _this->_stdout;
		}

		std::istream &err()
		{
			return _this->_stderr;
		}

		int wait_for()
		{
			if (_this->_exit_code.has_value()) {
				return _this->_exit_code.value();
			}
			_this->_exit_code = mpp_impl::wait_for(_this->_info);
			return _this->_exit_code.value();
		}

		/**
		 * Wait up to timeout_ms for the process to exit.
		 * poll_interval_ms controls the sleep between polls on Unix (minimum 1 ms);
		 * on Windows the OS signals process exit natively and this value is ignored.
		 * Returns the exit code wrapped in optional if exited, or nullopt on timeout.
		 * Note: CNI layer no longer calls this; it is kept as a public API.
		 */
		std::optional<int> wait_timeout_ms(int timeout_ms, int poll_interval_ms = 5)
		{
			if (_this->_exit_code.has_value()) {
				return _this->_exit_code;
			}
			int code = 0;
			if (mpp_impl::wait_timeout_ms(_this->_info, timeout_ms, code, poll_interval_ms)) {
				_this->_exit_code = code;
				return code;
			}
			return std::nullopt;
		}

		/**
		 * Start a background OS thread that performs a single blocking
		 * mpp_impl::wait_for() and stores the exit code in _wait_future.
		 * Idempotent: safe to call multiple times.
		 */
		void begin_wait()
		{
			if (_this->_exit_code.has_value() || _this->_wait_future.valid()) {
				return;
			}
			auto *impl = _this.get();
			impl->_wait_future = std::async(std::launch::async, [impl]() {
				return mpp_impl::wait_for(impl->_info);
			});
		}

		/**
		 * Non-blocking poll of the wait future.
		 * Returns true when the process has exited (or was already collected).
		 * Zero syscalls when the future is not yet ready; caches result in _exit_code.
		 */
		bool poll_wait()
		{
			if (_this->_exit_code.has_value()) {
				return true;
			}
			if (!_this->_wait_future.valid()) {
				// No async wait started; fall back to a direct (syscall) check.
				if (mpp_impl::process_exited(_this->_info)) {
					_this->_observed_exited = true;
					return true;
				}
				return false;
			}
			if (_this->_wait_future.wait_for(std::chrono::milliseconds(0))
			        == std::future_status::ready) {
				_this->_exit_code = _this->_wait_future.get();
				return true;
			}
			return false;
		}

		/**
		 * Block until the wait future resolves (if it was started), then cache and
		 * return the exit code.  Falls back to direct wait_for() if begin_wait() was
		 * never called.
		 */
		int collect_wait()
		{
			if (_this->_exit_code.has_value()) {
				return _this->_exit_code.value();
			}
			if (_this->_wait_future.valid()) {
				_this->_exit_code = _this->_wait_future.get();
				return _this->_exit_code.value();
			}
			// Fallback: no async wait was started, do it synchronously.
			return wait_for();
		}

		bool has_exited()
		{
			if (_this->_exit_code.has_value() || _this->_observed_exited) {
				return true;
			}
			// If an async wait is in progress, check it without a syscall.
			if (_this->_wait_future.valid()) {
				if (_this->_wait_future.wait_for(std::chrono::milliseconds(0))
				        == std::future_status::ready) {
					_this->_exit_code = _this->_wait_future.get();
					return true;
				}
				return false;
			}
			// No async wait: fall back to the OS-level non-blocking check.
			if (mpp_impl::process_exited(_this->_info)) {
				_this->_observed_exited = true;
				return true;
			}
			return false;
		}

		void interrupt(bool force = false)
		{
			mpp_impl::terminate_process(_this->_info, force);
		}

		void interrupt_tree(bool force = false)
		{
			mpp_impl::terminate_process_tree(_this->_info, force);
		}

		/**
		 * Return the OS-level process ID.
		 */
		int pid() const
		{
			return mpp_impl::get_pid(_this->_info);
		}

		/**
		 * Drain stdout and stderr simultaneously (to avoid pipe-full deadlocks)
		 * and wait for the process to exit.
		 *
		 * Returns {stdout_string, stderr_string, exit_code}.
		 * If a stream was not captured (inherit or merge mode), the
		 * corresponding string will be empty.
		 */
		struct communicate_result {
			std::string out;
			std::string err;
			int exit_code = 0;
		};

		/**
		 * Launch reader futures for stdout/stderr and a waiter future; returns
		 * immediately.  Captures a stable raw pointer to member_holder so the
		 * caller may freely move or re-seat the shared_ptr<process> without
		 * invalidating in-flight captures.  Must be followed by end_communicate()
		 * before the next call; the destructor safely joins any outstanding future.
		 */
		void begin_communicate()
		{
			// Start the exit waiter in parallel with the IO readers so that process
			// exit and pipe drain race concurrently rather than sequentially.
			begin_wait();
			auto *impl = _this.get();
			if (impl->_info._stdout != FD_INVALID) {
				impl->_out_future = std::async(std::launch::async, [impl]() {
					std::ostringstream ss;
					ss << impl->_stdout.rdbuf();
					return ss.str();
				});
			}
			if (impl->_info._stderr != FD_INVALID) {
				impl->_err_future = std::async(std::launch::async, [impl]() {
					std::ostringstream ss;
					ss << impl->_stderr.rdbuf();
					return ss.str();
				});
			}
		}

		/**
		 * Non-blocking poll: returns true when both reader futures are done.
		 * Safe to call from a yield loop without blocking the OS thread.
		 */
		bool poll_communicate()
		{
			constexpr auto zero = std::chrono::milliseconds(0);
			bool out_ok = !_this->_out_future.valid() ||
			              _this->_out_future.wait_for(zero) == std::future_status::ready;
			bool err_ok = !_this->_err_future.valid() ||
			              _this->_err_future.wait_for(zero) == std::future_status::ready;
			return out_ok && err_ok;
		}

		/**
		 * Collect reader results and wait for process exit.
		 * Call after poll_communicate() returns true, or to block-wait directly.
		 */
		communicate_result end_communicate()
		{
			communicate_result result;
			if (_this->_out_future.valid()) result.out = _this->_out_future.get();
			if (_this->_err_future.valid()) result.err = _this->_err_future.get();
			result.exit_code = collect_wait();
			return result;
		}

		/**
		 * Blocking convenience wrapper (original semantics preserved):
		 * begin + block for both readers + collect.
		 */
		communicate_result communicate()
		{
			begin_communicate();
			if (_this->_out_future.valid()) _this->_out_future.wait();
			if (_this->_err_future.valid()) _this->_err_future.wait();
			return end_communicate();
		}

	public:
		static process exec(const std::string &command);

		static process exec(const std::string &command,
		                    const std::vector<std::string> &args);
	};

	class process_builder {
	private:
		process_startup _startup;

	public:
		process_builder() = default;

		~process_builder() = default;

		process_builder(process_builder &&) = default;

		process_builder(const process_builder &) = default;

		process_builder &operator=(process_builder &&) = default;

		process_builder &operator=(const process_builder &) = default;

	public:
		/**
		 * Set the program to execute (argv[0]). May be called multiple times;
		 * later calls replace the previous program.
		 */
		process_builder &command(const std::string &command)
		{
			if (_startup._cmdline.empty()) {
				_startup._cmdline.push_back(command);
			}
			else {
				_startup._cmdline[0].assign(command);
			}
			return *this;
		}

		/**
		 * Append arguments after the program name. May be called AT MOST ONCE
		 * per builder; a second call throws mpp::runtime_error.
		 */
		template <typename Container>
		process_builder &arguments(const Container &c)
		{
			if (_startup._cmdline.size() > 1) {
				mpp::throw_ex<mpp::runtime_error>("arguments() must be called at most once per process_builder");
			}
			std::copy(c.begin(), c.end(), std::back_inserter(_startup._cmdline));
			return *this;
		}

		process_builder &environment(const std::string &key, const std::string &value)
		{
			_startup._env.emplace(key, value);
			return *this;
		}

		process_builder &redirect_stdin(fd_type target)
		{
			_startup._stdin._target = target;
			return *this;
		}

		process_builder &redirect_stdout(fd_type target)
		{
			_startup._stdout._target = target;
			return *this;
		}

		process_builder &redirect_stderr(fd_type target)
		{
			_startup._stderr._target = target;
			return *this;
		}

		process_builder &directory(const std::string &cwd)
		{
			_startup._cwd = cwd;
			return *this;
		}

		process_builder &merge_outputs(bool r)
		{
			_startup.merge_outputs = r;
			return *this;
		}

		/**
		 * Inherit the parent's stdin from the terminal (user can type directly).
		 */
		process_builder &inherit_stdin(bool v = true)
		{
			_startup.inherit_stdin = v;
			return *this;
		}

		/**
		 * Inherit the parent's stdout and stderr so child output appears
		 * directly on the terminal without being captured.
		 */
		process_builder &inherit_output(bool v = true)
		{
			_startup.inherit_stdout = v;
			_startup.inherit_stderr = v;
			return *this;
		}

		/**
		 * Control environment inheritance.  When true (the default) the child
		 * receives the parent's full environment, with any vars set via
		 * environment() applied as overrides.  When false only those vars are
		 * used (or an empty environment if none were set).
		 */
		process_builder &inherit_env(bool v = true)
		{
			_startup._inherit_env = v;
			return *this;
		}

		process_builder &shell(std::nullptr_t)
		{
			_startup.shell_mode = false;
			_startup._shell_program.reset();
			return *this;
		}

		process_builder &shell(const std::string &program)
		{
			if (program.empty()) {
				mpp::throw_ex<mpp::runtime_error>("shell program cannot be empty");
			}
			_startup.shell_mode = true;
			_startup._shell_program = program;
			return *this;
		}

		process start()
		{
			process_startup s = _startup;
			if (s.shell_mode) {
				if (s._cmdline.empty()) {
					mpp::throw_ex<mpp::runtime_error>("no command specified");
				}
				std::string cmd;
				for (size_t i = 0; i < s._cmdline.size(); ++i) {
					if (i > 0) cmd += ' ';
					cmd += s._cmdline[i];
				}
#ifdef MOZART_PLATFORM_WIN32
				const std::string shell_program = s._shell_program.value_or("cmd");
				s._cmdline = {shell_program, "/c", std::move(cmd)};
#else
				const std::string shell_program = s._shell_program.value_or("/bin/sh");
				s._cmdline = {shell_program, "-c", std::move(cmd)};
#endif
			}
			else if (s._cmdline.empty()) {
				mpp::throw_ex<mpp::runtime_error>("no command specified");
			}
			process_info info{};
			mpp_impl::create_process(s, info);
			return process(info);
		}
	};
}
