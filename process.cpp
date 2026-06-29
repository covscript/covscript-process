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

// Heap-allocated bundle for asynchronous filesystem requests.
// uv_wait_fs_with_deadline always drains the request to completion
// before returning, so the caller can safely delete the bundle
// immediately after the wait returns — no deferred cleanup is needed.
struct uv_fs_request {
	uv_fs_t req;
	uv_fs_op_state state;
	std::string write_data;  // owned copy for write buffer lifetime
};

static void uv_fs_complete(uv_fs_t *req)
{
	auto *bundle = static_cast<uv_fs_request *>(req->data);
	bundle->state.result = static_cast<int>(req->result);
	bundle->state.done = true;
	uv_fs_req_cleanup(req);
}

// Wait for a filesystem request to complete, with an optional deadline.
//
// Returns true if the operation completed before the deadline.
// Returns false if the deadline was reached (even if cancel failed and we
// had to wait for completion).
//
// When uv_cancel fails with UV_EBUSY the request is already executing;
// we continue driving the loop until completion to ensure the callback
// fires and cleans up libuv resources. The caller always owns the bundle
// after this function returns.
static bool uv_wait_fs_with_deadline(uv_loop_t *loop, uv_fs_request *bundle,
                                     int deadline_ms)
{
	bool deadline_reached = false;
	const bool has_deadline = deadline_ms >= 0;
	const auto deadline = std::chrono::steady_clock::now()
	                      + std::chrono::milliseconds(has_deadline ? deadline_ms : 0);

	while (!bundle->state.done) {
		uv_run(loop, UV_RUN_NOWAIT);
		if (bundle->state.done) break;

		if (has_deadline && !deadline_reached
		        && std::chrono::steady_clock::now() >= deadline) {
			deadline_reached = true;
			const int cancel_rc = uv_cancel(
			                          reinterpret_cast<uv_req_t *>(&bundle->req));
			if (cancel_rc == 0) {
				// Cancel succeeded — callback fires with UV_ECANCELED.
				// Continue looping to collect the callback.
				continue;
			}
			// Cancel failed (UV_EBUSY): request is already executing.
			// Continue driving the loop until completion so the callback
			// fires and cleans up libuv resources.
		}

		cs_runtime_yield(1);
	}

	return !deadline_reached;
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

static std::string get_default_shell()
{
#ifdef MOZART_PLATFORM_WIN32
	const char *shell = std::getenv("COMSPEC");
	return shell ? shell : "cmd";
#else
	const char *shell = std::getenv("SHELL");
	return shell ? shell : "/bin/sh";
#endif
}

CNI_ROOT_NAMESPACE {
	CNI_V(exec, [](const std::string &cmd, const cs::array &args)
	{
		std::vector<std::string> arr;
		for (auto &it:args)
			arr.emplace_back(it.const_val<std::string>());
		return std::make_shared<mpp::process>(mpp::process::exec(cmd, arr));
	})

	CNI_V(default_shell, []() -> std::string
	{
		return get_default_shell();
	})

	CNI_V(shell, [](const std::string &command)
	{
		builder_t b;
		b.command(command);
		b.shell(get_default_shell());
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
			auto *bundle = new uv_fs_request{};
			bundle->req.data = bundle;
			const uv_file ufd = to_uv_file(f);
			if (ufd < 0)
			{
				delete bundle;
				return cs::null_pointer;
			}

			const int submit = uv_fs_read(
			                       uv_default_loop(), &bundle->req, ufd, &iov, 1,
			                       static_cast<int64_t>(f->read_position()), uv_fs_complete);
			if (submit < 0)
			{
				uv_fs_req_cleanup(&bundle->req);
				delete bundle;
				return cs::null_pointer;
			}

			const bool on_time = uv_wait_fs_with_deadline(
			                         uv_default_loop(), bundle, deadline_ms);
			const int n = bundle->state.result;
			delete bundle;
			if (n > 0)
			{
				// When the deadline was exceeded the caller receives null and
				// may retry — do not advance the read position so the retry
				// re-reads from the same offset.  The explicit offset passed to
				// uv_fs_read makes this safe regardless of OS file position.
				if (!on_time)
					return cs::null_pointer;
				f->advance_read(n);
				return cs::var::make<std::string>(std::string(buf.data(), n));
			}
			// n == 0: EOF — return empty string even if the deadline was
			// reached, so callers can distinguish EOF from timeout/error.
			if (n == 0)
				return cs::var::make<std::string>(std::string{});
			// n < 0: error or timeout.
			return cs::null_pointer;
		})
		// write(data, deadline_ms): write data to the file.
		// Returns bytes written, or -1 on error / timeout.
		//
		// When a deadline is set and the write is still executing when the
		// deadline fires, uv_cancel races with the worker thread. If cancel
		// wins the data is never written; if the worker wins the OS write
		// completes but we return -1 anyway because the caller has already
		// exceeded its time budget. Callers that retry on -1 should be aware
		// that the original write may have partially or fully succeeded.
		CNI_V(write, [](file_t &f, const std::string &data, int deadline_ms) -> int {
			if (!f || !f->is_writable()) return -1;

			const bool has_deadline = deadline_ms >= 0;
			// Copy write data into the bundle when a deadline is set so the
			// buffer remains valid even if we need to wait past the deadline.
			// Without a deadline we always wait for completion, so the
			// caller's string reference is safe.
			std::string data_copy;
			if (has_deadline)
				data_copy = data;
			auto *bundle = new uv_fs_request{};
			bundle->req.data = bundle;
			bundle->write_data = std::move(data_copy);
			const char *buf_ptr = has_deadline ? bundle->write_data.data() : data.data();
			const auto buf_size = has_deadline ? bundle->write_data.size() : data.size();

			// uv_buf_init() takes unsigned int; reject oversized buffers
			// to prevent silent truncation.
			if (buf_size > UINT_MAX)
			{
				delete bundle;
				return -1;
			}
			uv_buf_t iov = uv_buf_init(const_cast<char *>(buf_ptr),
			                           static_cast<unsigned int>(buf_size));
			const uv_file ufd = to_uv_file(f);
			if (ufd < 0)
			{
				delete bundle;
				return -1;
			}

			// In append mode, pass -1 so the OS writes at the end of the file.
			// Using write_position() would overwrite from position 0 instead.
			const int64_t offset = f->is_append() ? -1 : f->write_position();
			const int submit = uv_fs_write(
			                       uv_default_loop(), &bundle->req, ufd, &iov, 1,
			                       offset, uv_fs_complete);
			if (submit < 0)
			{
				uv_fs_req_cleanup(&bundle->req);
				delete bundle;
				return -1;
			}

			const bool on_time = uv_wait_fs_with_deadline(
			                         uv_default_loop(), bundle, deadline_ms);
			const int result = bundle->state.result;
			delete bundle;
			if (result > 0)
			{
				// When the deadline was exceeded the caller receives -1 and
				// may retry — do not advance the write position so the retry
				// writes to the same offset (overwriting the timed-out data).
				if (!on_time)
					return -1;
				if (!f->is_append())
					f->advance_write(result);
				return result;
			}
			if (!on_time)
				return -1;
			return result;
		})
		// flush(deadline_ms): flush write buffers. Returns true on success.
		//
		// When a deadline is set and fsync is still executing when the
		// deadline fires, the same cancel-vs-worker race described in
		// write() applies: if the worker wins the fsync completes but we
		// return false because the caller has exceeded its time budget.
		CNI_V(flush, [](file_t &f, int deadline_ms) -> bool {
			if (!f || !f->is_writable()) return false;

			auto *bundle = new uv_fs_request{};
			bundle->req.data = bundle;
			const uv_file ufd = to_uv_file(f);
			if (ufd < 0)
			{
				delete bundle;
				return false;
			}

			const int submit = uv_fs_fsync(
			                       uv_default_loop(), &bundle->req, ufd, uv_fs_complete);
			if (submit < 0)
			{
				uv_fs_req_cleanup(&bundle->req);
				delete bundle;
				return false;
			}

			const bool on_time = uv_wait_fs_with_deadline(
			                         uv_default_loop(), bundle, deadline_ms);
			const bool ok = bundle->state.result >= 0;
			delete bundle;
			// Return true only when fsync succeeded AND completed before
			// the deadline.  When deadline_ms < 0 (no deadline), on_time
			// is always true, so this reduces to `ok`.
			return ok && on_time;
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
		CNI_V(cmd, [](const cs::var &b, const std::string &str) -> cs::var {
			b.val<builder_t>().command(str);
			return b;
		})
		CNI_V(arg, [](const cs::var &b, const cs::array &args) -> cs::var {
			std::vector<std::string> arr;
			for (auto &it:args)
				arr.emplace_back(it.const_val<std::string>());
			b.val<builder_t>().arguments(arr);
			return b;
		})
		CNI_V(dir, [](const cs::var &b, const std::string &str) -> cs::var {
			b.val<builder_t>().directory(str);
			return b;
		})
		CNI_V(env, [](const cs::var &b, const std::string &key, const std::string &value) -> cs::var {
			b.val<builder_t>().environment(key, value);
			return b;
		})
		CNI_V(merge_output, [](const cs::var &b, bool r) -> cs::var {
			b.val<builder_t>().merge_outputs(r);
			return b;
		})
		CNI_V(inherit_output, [](const cs::var &b, bool v) -> cs::var {
			b.val<builder_t>().inherit_output(v);
			return b;
		})
		CNI_V(inherit_stdin, [](const cs::var &b, bool v) -> cs::var {
			b.val<builder_t>().inherit_stdin(v);
			return b;
		})
		CNI_V(inherit_stdout, [](const cs::var &b, bool v) -> cs::var {
			b.val<builder_t>().inherit_stdout(v);
			return b;
		})
		CNI_V(inherit_stderr, [](const cs::var &b, bool v) -> cs::var {
			b.val<builder_t>().inherit_stderr(v);
			return b;
		})
		CNI_V(inherit_env, [](const cs::var &b, bool v) -> cs::var {
			b.val<builder_t>().inherit_env(v);
			return b;
		})
		CNI_V(shell, [](const cs::var &b, const std::string &program) -> cs::var {
			b.val<builder_t>().shell(program);
			return b;
		})
		// redirect_in(file_t): redirect child stdin from a file_t opened for reading.
		CNI_V(redirect_in, [](const cs::var &b, const file_t &f) -> cs::var {
			if (!f || !f->is_readable())
				mpp::throw_ex<mpp::runtime_error>("file_t is not open for reading");
			b.val<builder_t>().redirect_stdin(f->native_fd());
			return b;
		})
		// redirect_out(file_t): redirect child stdout to a file_t opened for writing.
		CNI_V(redirect_out, [](const cs::var &b, const file_t &f) -> cs::var {
			if (!f || !f->is_writable())
				mpp::throw_ex<mpp::runtime_error>("file_t is not open for writing");
			b.val<builder_t>().redirect_stdout(f->native_fd());
			return b;
		})
		// redirect_err(file_t): redirect child stderr to a file_t opened for writing.
		CNI_V(redirect_err, [](const cs::var &b, const file_t &f) -> cs::var {
			if (!f || !f->is_writable())
				mpp::throw_ex<mpp::runtime_error>("file_t is not open for writing");
			b.val<builder_t>().redirect_stderr(f->native_fd());
			return b;
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
				if (p->poll_wait()) {
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
				if (p->poll_wait()) {
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
