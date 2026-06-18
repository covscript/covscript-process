/*
* Covariant Script Libmozart++ Process Support
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* Copyright (C) 2017-2026 Michael Lee(李登淳)
*
* Email:   mikecovlee@163.com
* Github:  https://github.com/mikecovlee
* Website: http://covscript.org.cn
*/

#include <covscript/dll.hpp>
#include <covscript/cni.hpp>
#include <mozart++/process>
#include <mozart++/file>

#include <uv.h>

#include <chrono>
#include <thread>

#ifdef MOZART_PLATFORM_WIN32
#include <io.h>
#include <fcntl.h>
#endif

// Cooperative-yield helper. When the host CovScript runtime supports fibers
// (ABI >= 250908 and <covscript/fiber.hpp>-equivalent symbols are reachable
// through the SDK headers we already include), let the current fiber yield to
// peer fibers instead of putting the OS thread to sleep. Otherwise fall back
// to the historical behaviour of sleeping for `fallback_sleep_ms` milliseconds.
//
// `cs::current_process` and `cs::fiber::yield()` are declared in
// covscript/core/core.hpp, which is transitively included via covscript/dll.hpp
// and covscript/cni.hpp above.
#define COVSCRIPT_PROCESS_FIBER_ABI_MIN 250908

#if defined(COVSCRIPT_ABI_VERSION) && COVSCRIPT_ABI_VERSION >= COVSCRIPT_PROCESS_FIBER_ABI_MIN
#define COVSCRIPT_PROCESS_HAVE_FIBER 1
#else
#define COVSCRIPT_PROCESS_HAVE_FIBER 0
#endif

static inline void cs_runtime_yield(int fallback_sleep_ms)
{
#if COVSCRIPT_PROCESS_HAVE_FIBER
	if (cs::current_process != nullptr && !cs::current_process->fiber_stack.empty()) {
		cs::fiber::yield();
		return;
	}
#endif
	if (fallback_sleep_ms > 0)
		std::this_thread::sleep_for(std::chrono::milliseconds(fallback_sleep_ms));
}

struct uv_fs_op_state {
	bool done = false;
	int result = UV_ECANCELED;
};

static inline void uv_fs_complete(uv_fs_t *req)
{
	auto *state = static_cast<uv_fs_op_state *>(req->data);
	state->result = static_cast<int>(req->result);
	state->done = true;
	uv_fs_req_cleanup(req);
}

static inline bool uv_wait_fs_with_deadline(uv_loop_t *loop, uv_fs_t *req,
	uv_fs_op_state &state, int deadline_ms)
{
	bool timed_out = false;
	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(deadline_ms < 0 ? 0 : deadline_ms);

	while (!state.done) {
		uv_run(loop, UV_RUN_NOWAIT);
		if (state.done) break;

		if (!timed_out && deadline_ms >= 0
			&& std::chrono::steady_clock::now() >= deadline) {
			timed_out = true;
			uv_cancel(reinterpret_cast<uv_req_t *>(req));
		}

		cs_runtime_yield(1);
	}

	return !timed_out;
}

static inline uv_file to_uv_file(const mpp::file_ptr &f)
{
#ifdef MOZART_PLATFORM_WIN32
	return static_cast<uv_file>(f->get_uv_fd());
#else
	return static_cast<uv_file>(f->native_fd());
#endif
}

using process_t = std::shared_ptr<mpp::process>;
using builder_t = mpp::process_builder;
using file_t = mpp::file_ptr;

CNI_ROOT_NAMESPACE {
	CNI_V(exec, [](const std::string &cmd, const cs::array &args)
	{
		std::vector<std::string> arr;
		for (auto &it:args)
			arr.emplace_back(it.const_val<std::string>());
		return std::make_shared<mpp::process>(mpp::process::exec(cmd, arr));
	})

	CNI_V(shell, [](const std::string &command)
	{
		builder_t b;
		b.command(command);
#ifdef MOZART_PLATFORM_WIN32
		b.shell("cmd");
#else
		b.shell("/bin/sh");
#endif
		return std::make_shared<mpp::process>(b.start());
	})

	// -------------------------------------------------------------------------
	// file_t extension methods
	// -------------------------------------------------------------------------
	CNI_TYPE_EXT_V(file_type, file_t, file, file_t())
	{
		CNI_V(close, [](file_t &f) {
			if (f) f->close_file();
		})
		CNI_V(is_readable, [](const file_t &f) -> bool {
			return f && f->is_readable();
		})
		CNI_V(is_writable, [](const file_t &f) -> bool {
			return f && f->is_writable();
		})
		// out() -> cs::istream for reading file content sequentially.
		CNI_V(out, [](file_t &f) -> cs::var {
			if (!f || !f->is_readable()) return cs::null_pointer;
			return cs::istream(&f->out_stream(), [](std::istream *) {});
		})
		// in() -> cs::ostream for writing to the file.
		CNI_V(in, [](file_t &f) -> cs::var {
			if (!f || !f->is_writable()) return cs::null_pointer;
			return cs::ostream(&f->in_stream(), [](std::ostream *) {});
		})
		// read(size, deadline_ms): read up to size bytes at current position.
		// Returns data string on success, empty string on EOF, null on error/timeout.
		// deadline_ms < 0: wait indefinitely; >= 0: wait up to that many ms.
		CNI_V(read, [](file_t &f, int size, int deadline_ms) -> cs::var {
			if (!f || !f->is_readable()) return cs::null_pointer;
			if (size <= 0) return cs::var::make<std::string>(std::string{});

			std::vector<char> buf(size);
			uv_buf_t iov = uv_buf_init(buf.data(), size);
			uv_fs_t req{};
			uv_fs_op_state state{};
			req.data = &state;
			const uv_file ufd = to_uv_file(f);
			if (ufd < 0) return cs::null_pointer;

			const int submit = uv_fs_read(
				uv_default_loop(), &req, ufd, &iov, 1,
				static_cast<int64_t>(f->read_position()), uv_fs_complete);
			if (submit < 0) {
				uv_fs_req_cleanup(&req);
				return cs::null_pointer;
			}
			if (!uv_wait_fs_with_deadline(uv_default_loop(), &req, state, deadline_ms))
				return cs::null_pointer;

			const int n = state.result;
			if (n > 0) {
				f->advance_read(n);
				return cs::var::make<std::string>(std::string(buf.data(), n));
			}
			if (n < 0) return cs::null_pointer;
			// n == 0: EOF
			return cs::var::make<std::string>(std::string{});
		})
		// write(data, deadline_ms): write data to the file.
		// Returns bytes written or -1 on error.
		CNI_V(write, [](file_t &f, const std::string &data, int deadline_ms) -> int {
			if (!f || !f->is_writable()) return -1;

			uv_buf_t iov = uv_buf_init(const_cast<char *>(data.data()), static_cast<unsigned int>(data.size()));
			uv_fs_t req{};
			uv_fs_op_state state{};
			req.data = &state;
			const uv_file ufd = to_uv_file(f);
			if (ufd < 0) return -1;

			// In append mode, pass -1 so the OS writes at the end of the file.
			// Using write_position() would overwrite from position 0 instead.
			const int64_t offset = f->is_append() ? -1 : f->write_position();
			const int submit = uv_fs_write(
				uv_default_loop(), &req, ufd, &iov, 1,
				offset, uv_fs_complete);
			if (submit < 0) {
				uv_fs_req_cleanup(&req);
				return -1;
			}
			if (!uv_wait_fs_with_deadline(uv_default_loop(), &req, state, deadline_ms))
				return -1;
			if (state.result > 0 && !f->is_append())
				f->advance_write(state.result);
			return state.result;
		})
		// flush(deadline_ms): flush write buffers. Returns true on success.
		CNI_V(flush, [](file_t &f, int deadline_ms) -> bool {
			if (!f || !f->is_writable()) return false;

			uv_fs_t req{};
			uv_fs_op_state state{};
			req.data = &state;
			const uv_file ufd = to_uv_file(f);
			if (ufd < 0) return false;

			const int submit = uv_fs_fsync(
				uv_default_loop(), &req, ufd, uv_fs_complete);
			if (submit < 0) {
				uv_fs_req_cleanup(&req);
				return false;
			}
			if (!uv_wait_fs_with_deadline(uv_default_loop(), &req, state, deadline_ms))
				return false;
			return state.result >= 0;
		})
	}

	// -------------------------------------------------------------------------
	// process.async — event loop + async file I/O
	// -------------------------------------------------------------------------
	CNI_NAMESPACE(async)
	{
		// Drive the libuv event loop without blocking.
		// Returns non-zero if the loop has active handles/requests remaining.
		CNI_V(poll, []() -> int {
			return uv_run(uv_default_loop(), UV_RUN_NOWAIT);
		})

		// Run one iteration of the event loop (may wait briefly for I/O).
		// Returns true if more work remains after this iteration.
		CNI_V(poll_once, []() -> bool {
			return uv_run(uv_default_loop(), UV_RUN_ONCE) != 0;
		})

		// Stop the currently-running event loop (uv_run returns after current callbacks).
		// Calling poll() / poll_once() afterwards resumes normally.
		CNI_V(stop, []() {
			uv_stop(uv_default_loop());
		})

		// Restart the event loop after a stop() call.
		// libuv resumes automatically when uv_run() is called again; this is
		// a readability marker / future hook for pre-restart setup.
		CNI_V(restart, []() {
			// uv_run() can be called again after uv_stop() without extra setup.
		})

		// Open or create a file at `path` with the given `mode`.
		// Modes: "r" (read), "w" (write/create/truncate), "a" (append),
		//        "r+" (read+write existing), "w+" (read+write/create/truncate)
		// Returns a file_t handle, or null on failure.
		CNI_V(fstream, [](const std::string &path, const std::string &mode) -> cs::var {
			auto f = mpp::open_file(path, mode);
			if (!f) return cs::null_pointer;
			return cs::var::make<file_t>(f);
		})
	}

	CNI_TYPE_EXT_V(builder_type, builder_t, builder, builder_t())
	{
		CNI_V(cmd, [](builder_t &b, const std::string &str) {
			b.command(str);
		})
		CNI_V(arg, [](builder_t &b, const cs::array &args) {
			std::vector<std::string> arr;
			for (auto &it:args)
				arr.emplace_back(it.const_val<std::string>());
			b.arguments(arr);
		})
		CNI_V(dir, [](builder_t &b, const std::string &str) {
			b.directory(str);
		})
		CNI_V(env, [](builder_t &b, const std::string &key, const std::string &value) {
			b.environment(key, value);
		})
		CNI_V(merge_output, [](builder_t &b, bool r) {
			b.merge_outputs(r);
		})
		CNI_V(inherit_output, [](builder_t &b, bool v) {
			b.inherit_output(v);
		})
		CNI_V(inherit_env, [](builder_t &b, bool v) {
			b.inherit_env(v);
		})
		// Alias for script environments where member name `shell` is not reachable.
		CNI_V(use_shell, [](builder_t &b, const std::string &program) {
			b.shell(program);
		})
		// redirect_in(file_t): redirect child stdin from a file_t opened for reading.
		CNI_V(redirect_in, [](builder_t &b, const file_t &f) {
			if (!f || !f->is_readable())
				mpp::throw_ex<mpp::runtime_error>("file_t is not open for reading");
			b.redirect_stdin(f->native_fd());
		})
		// redirect_out(file_t): redirect child stdout to a file_t opened for writing.
		CNI_V(redirect_out, [](builder_t &b, const file_t &f) {
			if (!f || !f->is_writable())
				mpp::throw_ex<mpp::runtime_error>("file_t is not open for writing");
			b.redirect_stdout(f->native_fd());
		})
		// redirect_err(file_t): redirect child stderr to a file_t opened for writing.
		CNI_V(redirect_err, [](builder_t &b, const file_t &f) {
			if (!f || !f->is_writable())
				mpp::throw_ex<mpp::runtime_error>("file_t is not open for writing");
			b.redirect_stderr(f->native_fd());
		})
		CNI_V(start, [](builder_t &b) {
			return std::make_shared<mpp::process>(b.start());
		})
	}

	CNI_NAMESPACE(process_type)
	{
		CNI_V(in, [](const process_t &p) {
			return cs::ostream(&p->in(), [](std::ostream *) {});
		})
		CNI_V(out, [](const process_t &p) {
			return cs::istream(&p->out(), [](std::istream *) {});
		})
		CNI_V(err, [](const process_t &p) {
			return cs::istream(&p->err(), [](std::istream *) {});
		})
		CNI_V(wait, [](const process_t &p) {
			// In a fiber context: launch begin_wait() so work on the libuv thread pool performs
			// the single blocking waitpid/WaitForSingleObject, then cooperatively
			// yield until poll_wait() sees the work complete — zero syscalls per tick.
			// Outside a fiber: collect_wait() falls back to direct synchronous wait.
#if COVSCRIPT_PROCESS_HAVE_FIBER
			if (cs::current_process != nullptr && !cs::current_process->fiber_stack.empty()) {
				p->begin_wait();
				while (!p->poll_wait())
					cs::fiber::yield();
				return p->collect_wait();
			}
#endif
			return p->collect_wait();
		})
		CNI_V(try_wait, [](const process_t &p) -> cs::var {
			// poll_wait() checks the work state without a syscall (if begin_wait()
			// was already called), or falls back to a single process_exited() check.
			if (p->poll_wait())
			{
				return cs::var::make<cs::numeric>(p->collect_wait());
			}
			return cs::null_pointer;
		})
		CNI_V(wait_poll, [](const process_t &p, long long timeout_ms, int poll_interval_ms) -> cs::var {
			// Launch the async waiter once so every subsequent poll_wait() call is
			// a zero-syscall status check (mutex only).
			p->begin_wait();
			const int interval = std::max(1, poll_interval_ms);
			// Already done (e.g. second call after process exited).
			if (p->poll_wait())
			{
				return cs::var::make<cs::numeric>(p->collect_wait());
			}
			if (timeout_ms < 0)
			{
				// Indefinite: poll until the work is done.
				while (!p->poll_wait())
					cs_runtime_yield(interval);
				return cs::var::make<cs::numeric>(p->collect_wait());
			}
			const auto deadline = std::chrono::steady_clock::now()
			                      + std::chrono::milliseconds(timeout_ms);
			while (std::chrono::steady_clock::now() < deadline)
			{
				cs_runtime_yield(interval);
				if (p->poll_wait())
				{
					return cs::var::make<cs::numeric>(p->collect_wait());
				}
			}
			return cs::null_pointer;
		})
		// wait_with(timeout_ms, on_wait): same polling semantics as wait_poll,
		// but the user-supplied callback `on_wait` is invoked once per iteration
		// in lieu of cs_runtime_yield(). The callback is responsible for any
		// yielding or sleeping. timeout_ms < 0 polls indefinitely.
		// begin_wait() is called first so poll_wait() iterations are syscall-free.
		CNI_V(wait_with, [](const process_t &p, long long timeout_ms,
		const cs::var &on_wait) -> cs::var {
			p->begin_wait();
			if (p->poll_wait())
			{
				return cs::var::make<cs::numeric>(p->collect_wait());
			}
			if (timeout_ms < 0)
			{
				// Indefinite: drive the callback loop until the work is done.
				while (!p->poll_wait())
					cs::invoke(on_wait);
				return cs::var::make<cs::numeric>(p->collect_wait());
			}
			const auto deadline = std::chrono::steady_clock::now()
			                      + std::chrono::milliseconds(timeout_ms);
			while (std::chrono::steady_clock::now() < deadline)
			{
				cs::invoke(on_wait);
				if (p->poll_wait())
				{
					return cs::var::make<cs::numeric>(p->collect_wait());
				}
			}
			return cs::null_pointer;
		})
		CNI_V(has_exited, [](const process_t &p) {
			return p->has_exited();
		})
		CNI_V(is_running, [](const process_t &p) {
			return !p->has_exited();
		})
		CNI_V(kill, [](const process_t &p, bool force) {
			p->interrupt(force);
		})
		CNI_V(kill_tree, [](const process_t &p, bool force) {
			p->interrupt_tree(force);
		})
		CNI_V(get_pid, [](const process_t &p) {
			return p->pid();
		})
		CNI_V(communicate, [](const process_t &p) {
			// Drains stdout and stderr simultaneously to avoid pipe-full deadlocks,
			// waits for the process to exit, and returns {stdout, stderr, exit_code}.
			// In a fiber context: begin_communicate() submits reader work to the libuv thread pool
			// (same thread overhead as always — two reader threads), then we poll
			// poll_communicate() + yield cooperatively until both readers finish.
			// No extra wrapper thread is needed; peer fibers stay schedulable.
#if COVSCRIPT_PROCESS_HAVE_FIBER
			if (cs::current_process != nullptr && !cs::current_process->fiber_stack.empty()) {
				p->begin_communicate();
				while (!p->poll_communicate())
					cs::fiber::yield();
				auto r = p->end_communicate();
				cs::array arr;
				arr.push_back(cs::var::make<std::string>(std::move(r.out)));
				arr.push_back(cs::var::make<std::string>(std::move(r.err)));
				arr.push_back(cs::var::make<cs::numeric>(r.exit_code));
				return arr;
			}
#endif
			auto r = p->communicate();
			cs::array arr;
			arr.push_back(cs::var::make<std::string>(std::move(r.out)));
			arr.push_back(cs::var::make<std::string>(std::move(r.err)));
			arr.push_back(cs::var::make<cs::numeric>(r.exit_code));
			return arr;
		})
	}
}

CNI_ENABLE_TYPE_EXT_V(file_type, file_t, process_file)
CNI_ENABLE_TYPE_EXT_V(builder_type, builder_t, process_builder)
CNI_ENABLE_TYPE_EXT_V(process_type, process_t, process)