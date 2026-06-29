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
#pragma once

#include <mozart++/core>
#include <mozart++/fdstream>
#include <optional>
#include <unordered_map>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <sstream>
#include <thread>
#include <vector>
#include <string>
#include <memory>
#include <atomic>

#include <uv.h>

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
		 * Stores pid_t (Unix) or process HANDLE (Win32).
		 * Use get_pid() for a uniform OS-level process ID.
		 */
		/**
		 * Unused on *nix systems.
		 */
		fd_type _tid = FD_INVALID;


		fd_type _pid = FD_INVALID;
		fd_type _stdin = FD_INVALID;
		fd_type _stdout = FD_INVALID;
		fd_type _stderr = FD_INVALID;
		bool _stdin_closed = false;
		/**
		 * Process creation timestamp for identity verification.
		 * Used to detect PID reuse before sending signals to process groups.
		 * 0 means "not recorded" (platform limitation or legacy process_info).
		 */
		uint64_t _start_time = 0;
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

	/**
	 * Read the creation timestamp of process @p pid for identity verification.
	 * Returns 0 when the information cannot be obtained (platform limitation,
	 * missing /proc, etc.).
	 *
	 * Linux:   reads /proc/<pid>/stat field 22 (starttime in clock ticks).
	 * macOS:   uses sysctl KERN_PROC_PID p_starttime (sec+usec packed).
	 * Windows: uses GetProcessTimes.
	 */
	uint64_t get_process_start_time(int pid);
}

namespace mpp {
	namespace detail {

		/**
		 * Opaque async-work state backed by libuv's thread pool (uv_queue_work).
		 *
		 * Replaces the previous std::async / std::future implementation, which
		 * spawned a fresh OS thread per operation.  libuv reuses a fixed-size
		 * thread pool (default 4 threads), integrates with the CovScript event
		 * loop, and lets poll_*() methods drive completion via uv_run() without
		 * syscalls on the hot path.
		 *
		 * Instances are allocated on the heap and owned by std::unique_ptr inside
		 * member_holder.  The work callback runs on a libuv thread-pool thread;
		 * the after-work callback runs on the loop thread when uv_run() is called.
		 */
		struct async_work {
			uv_work_t req;
			mpp_impl::process_info *info = nullptr;
			std::istream *stream = nullptr;
			std::atomic<bool> done{false};
			int exit_code = 0;
			std::string output;
		};

// Work callbacks (stateless lambdas → implicit conversion to fn ptr).
		inline void wait_work_cb(uv_work_t *req)
		{
			auto *w = static_cast<async_work *>(req->data);
			w->exit_code = mpp_impl::wait_for(*w->info);
		}

		inline void read_work_cb(uv_work_t *req)
		{
			auto *w = static_cast<async_work *>(req->data);
			assert(w->stream != nullptr);
			std::ostringstream ss;
			ss << w->stream->rdbuf();
			w->output = ss.str();
		}

		inline void after_work_cb(uv_work_t *req, int /*status*/)
		{
			auto *w = static_cast<async_work *>(req->data);
			w->done.store(true, std::memory_order_release);
		}

	} // namespace detail
} // namespace mpp

namespace mpp {
	using mpp_impl::redirect_info;
	using mpp_impl::process_info;
	using mpp_impl::process_startup;
	using mpp_impl::fd_type;

	// Thread safety: mpp::process is not thread-safe. All methods must be
	// called from the same thread that drives the libuv event loop
	// (uv_default_loop()). Multi-threaded access requires external
	// synchronization.

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
			// Async work states backed by libuv uv_queue_work, replacing the
			// previous std::async / std::future implementation.  Each pointer
			// is non-null when the corresponding async operation is in flight.
			std::unique_ptr<detail::async_work> _out_work;
			std::unique_ptr<detail::async_work> _err_work;
			std::unique_ptr<detail::async_work> _wait_work;

			// Helper: wait for a single work item to finish (or cancel it),
			// then release the unique_ptr.
			static void await_work(std::unique_ptr<detail::async_work> &w)
			{
				if (!w) return;
				uv_cancel(reinterpret_cast<uv_req_t *>(&w->req));
				int spin_count = 0;
				while (!w->done.load(std::memory_order_acquire)) {
					uv_run(uv_default_loop(), UV_RUN_NOWAIT);
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					if (++spin_count % 3000 == 0) {
						MOZART_LOGCR("await_work spinning for too long -- "
						             "work item not responding to cancellation");
					}
				}
				w.reset();
			}

			explicit member_holder(const process_info &info)
				: _info(info), _stdin(_info._stdin),
				  _stdout(_info._stdout), _stderr(_info._stderr) {}

			~member_holder()
			{
				// Cancel / join any in-flight async work before closing fds.
				await_work(_wait_work);
				await_work(_out_work);
				await_work(_err_work);
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

		process &operator=(process &&other) noexcept
		{
			if (this != &other)
				_this = std::move(other._this);
			return *this;
		}

		process &operator=(const process &) = delete;

	public:
		~process() = default;

		/**
		 * Returns the write end of the child's stdin pipe.
		 * Data written here becomes the child process's standard input.
		 * Returns a reference to an internally-owned fdostream.
		 */
		std::ostream &in()
		{
			return _this->_stdin;
		}

		/**
		 * Returns the read end of the child's stdout pipe.
		 * Read from here to consume the child process's standard output.
		 * Returns a reference to an internally-owned fdistream.
		 */
		std::istream &out()
		{
			return _this->_stdout;
		}

		/**
		 * Returns the read end of the child's stderr pipe.
		 * Read from here to consume the child process's standard error.
		 * Returns a reference to an internally-owned fdistream.
		 */
		std::istream &err()
		{
			return _this->_stderr;
		}

		/**
		 * Close the write end of the child's stdin pipe, signaling EOF to
		 * the child process.  Idempotent — safe to call multiple times.
		 * After this call, writing to in() has no effect.
		 */
		void close_stdin()
		{
			if (_this->_info._stdin_closed) return;
			_this->_info._stdin_closed = true;
			// Invalidate the stream buffer so subsequent writes are
			// safely refused instead of hitting a stale/closed fd
			// (the underlying streambuf returns EOF / 0, which sets
			// the stream's failbit / badbit).
			_this->_stdin.invalidate();
			if (_this->_info._stdin != FD_INVALID) {
				mpp_impl::close_fd(_this->_info._stdin);
				_this->_info._stdin = FD_INVALID;
			}
		}

		/**
		 * Wait up to timeout_ms for the process to exit.
		 * poll_interval_ms controls the sleep between polls on Unix (minimum 1 ms);
		 * on Windows the OS signals process exit natively and this value is ignored.
		 * Returns the exit code wrapped in optional if exited, or nullopt on timeout.
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
		 * Submit a blocking mpp_impl::wait_for() to libuv's thread pool.
		 * The result is collected via poll_wait() / collect_wait().
		 * Idempotent: safe to call multiple times.
		 */
		void begin_wait()
		{
			if (_this->_exit_code.has_value() || _this->_wait_work) {
				return;
			}
			auto w = std::make_unique<detail::async_work>();
			w->req.data = w.get();
			w->info = &_this->_info;
			if (uv_queue_work(uv_default_loop(), &w->req,
			                  detail::wait_work_cb, detail::after_work_cb) != 0) {
				MOZART_LOGEV("begin_wait: uv_queue_work submission failed, "
				             "falling back to synchronous path");
				return; // submission failed, w is freed
			}
			_this->_wait_work = std::move(w);
		}

		/**
		 * Non-blocking poll: drive the libuv loop to process pending
		 * completion callbacks, then check whether the wait work is done.
		 * Returns true when the process has exited (or was already collected).
		 * Zero syscalls when no work is pending; caches result in _exit_code.
		 */
		bool poll_wait()
		{
			if (_this->_exit_code.has_value() || _this->_observed_exited) {
				return true;
			}
			if (!_this->_wait_work) {
				// No async wait started; fall back to a direct (syscall) check.
				if (mpp_impl::process_exited(_this->_info)) {
					_this->_observed_exited = true;
					return true;
				}
				return false;
			}
			// Drive the loop so the after-work callback (which sets `done`)
			// gets a chance to fire.
			uv_run(uv_default_loop(), UV_RUN_NOWAIT);
			if (_this->_wait_work->done.load(std::memory_order_acquire)) {
				_this->_exit_code = _this->_wait_work->exit_code;
				_this->_wait_work.reset();
				_this->_observed_exited = true;
				return true;
			}
			return false;
		}

		/**
		 * Block until the libuv wait work resolves (if it was started),
		 * then cache and return the exit code.  Falls back to a direct
		 * synchronous mpp_impl::wait_for() if begin_wait() was never called.
		 */
		int collect_wait()
		{
			if (_this->_exit_code.has_value()) {
				return _this->_exit_code.value();
			}
			if (_this->_wait_work) {
				// Drive the loop until the work completes.
				while (!_this->_wait_work->done.load(std::memory_order_acquire)) {
					uv_run(uv_default_loop(), UV_RUN_NOWAIT);
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				_this->_exit_code = _this->_wait_work->exit_code;
				_this->_wait_work.reset();
				_this->_observed_exited = true;
				return _this->_exit_code.value();
			}
			// Fallback: no async wait was started, do it synchronously.
			_this->_exit_code = mpp_impl::wait_for(_this->_info);
			return _this->_exit_code.value();
		}

		bool has_exited()
		{
			if (_this->_exit_code.has_value() || _this->_observed_exited) {
				return true;
			}
			// If an async wait is in progress, drive the loop and check.
			if (_this->_wait_work) {
				uv_run(uv_default_loop(), UV_RUN_NOWAIT);
				if (_this->_wait_work->done.load(std::memory_order_acquire)) {
					_this->_exit_code = _this->_wait_work->exit_code;
					_this->_wait_work.reset();
					_this->_observed_exited = true;
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
		 * Submit reader work for stdout/stderr and a waiter work to libuv's
		 * thread pool; returns immediately.  Must be followed by
		 * end_communicate() before the next call.
		 *
		 * The async_work objects store raw pointers into member_holder
		 * (e.g. w->stream = &impl->_stdout).  These pointers remain valid
		 * because ~member_holder() calls await_work() on every outstanding
		 * async_work before destroying any stream or fd member — the
		 * async work is always joined before the pointee is freed.
		 */
		void begin_communicate()
		{
			// Close stdin so the child sees EOF and can exit naturally.
			close_stdin();
			// Start the exit waiter in parallel with the IO readers.
			begin_wait();
			auto *impl = _this.get();
			if (impl->_info._stdout != FD_INVALID && !impl->_out_work) {
				auto w = std::make_unique<detail::async_work>();
				w->req.data = w.get();
				w->stream = &impl->_stdout;
				if (uv_queue_work(uv_default_loop(), &w->req,
				                  detail::read_work_cb, detail::after_work_cb) == 0) {
					impl->_out_work = std::move(w);
				}
				else {
					MOZART_LOGEV("begin_communicate: uv_queue_work submission "
					             "failed for stdout reader");
				}
			}
			if (impl->_info._stderr != FD_INVALID && !impl->_err_work) {
				auto w = std::make_unique<detail::async_work>();
				w->req.data = w.get();
				w->stream = &impl->_stderr;
				if (uv_queue_work(uv_default_loop(), &w->req,
				                  detail::read_work_cb, detail::after_work_cb) == 0) {
					impl->_err_work = std::move(w);
				}
				else {
					MOZART_LOGEV("begin_communicate: uv_queue_work submission "
					             "failed for stderr reader");
				}
			}
		}

		/**
		 * Non-blocking poll: drive the libuv loop and return true when
		 * both reader-work items have completed.
		 * Safe to call from a yield loop without blocking the OS thread.
		 */
		bool poll_communicate()
		{
			uv_run(uv_default_loop(), UV_RUN_NOWAIT);
			bool out_ok = !_this->_out_work ||
			              _this->_out_work->done.load(std::memory_order_acquire);
			bool err_ok = !_this->_err_work ||
			              _this->_err_work->done.load(std::memory_order_acquire);
			return out_ok && err_ok;
		}

		/**
		 * Collect reader results and wait for process exit.
		 * Call after poll_communicate() returns true, or to block-wait directly.
		 */
		communicate_result end_communicate()
		{
			communicate_result result;
			if (_this->_out_work) {
				while (!_this->_out_work->done.load(std::memory_order_acquire)) {
					uv_run(uv_default_loop(), UV_RUN_NOWAIT);
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				result.out = std::move(_this->_out_work->output);
				_this->_out_work.reset();
			}
			if (_this->_err_work) {
				while (!_this->_err_work->done.load(std::memory_order_acquire)) {
					uv_run(uv_default_loop(), UV_RUN_NOWAIT);
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				result.err = std::move(_this->_err_work->output);
				_this->_err_work.reset();
			}
			result.exit_code = collect_wait();
			return result;
		}

		/**
		 * Blocking convenience wrapper (original semantics preserved):
		 * begin + drive loop until both readers finish + collect.
		 *
		 * Delegates to end_communicate() which already drives uv_run and
		 * collects results — no need to duplicate the wait loops here.
		 */
		communicate_result communicate()
		{
			begin_communicate();
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
		 * Append arguments after the program name.  May be called multiple
		 * times; later calls replace previously set arguments (last-wins).
		 */
		template <typename Container>
		process_builder &arguments(const Container &c)
		{
			if (_startup._cmdline.size() > 1)
				_startup._cmdline.erase(_startup._cmdline.begin() + 1, _startup._cmdline.end());
			std::copy(c.begin(), c.end(), std::back_inserter(_startup._cmdline));
			return *this;
		}

		/**
		 * Set an environment variable for the child process.  May be called
		 * multiple times; later values for the same key overwrite earlier
		 * ones (last-write-wins).
		 */
		process_builder &environment(const std::string &key, const std::string &value)
		{
			_startup._env.insert_or_assign(key, value);
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
		 * Inherit the parent's stdout independently (without affecting stderr).
		 * For setting both at once, see inherit_output().
		 */
		process_builder &inherit_stdout(bool v = true)
		{
			_startup.inherit_stdout = v;
			return *this;
		}

		/**
		 * Inherit the parent's stderr independently (without affecting stdout).
		 * For setting both at once, see inherit_output().
		 */
		process_builder &inherit_stderr(bool v = true)
		{
			_startup.inherit_stderr = v;
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
