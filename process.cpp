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
* Copyright (C) 2017-2021 Michael Lee(李登淳)
*
* Email:   lee@covariant.cn, mikecovlee@163.com
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

	// Alias for script environments where member name `shell` is not reachable.
	CNI_V(sh, [](const std::string &command)
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
			return f && f->readable && !f->closed;
		})
		CNI_V(is_writable, [](const file_t &f) -> bool {
			return f && f->writable && !f->closed;
		})
		// out() -> cs::istream for reading file content sequentially.
		CNI_V(out, [](file_t &f) -> cs::var {
			if (!f || f->closed || !f->readable) return cs::null_pointer;
			return cs::istream(&f->out_stream(), [](std::istream *) {});
		})
		// in() -> cs::ostream for writing to the file.
		CNI_V(in, [](file_t &f) -> cs::var {
			if (!f || f->closed || !f->writable) return cs::null_pointer;
			return cs::ostream(&f->in_stream(), [](std::ostream *) {});
		})
		// read(size, deadline_ms): read up to size bytes at current position.
		// Returns data string on success, empty string on EOF, null on error/timeout.
		// deadline_ms < 0: wait indefinitely; >= 0: wait up to that many ms.
		CNI_V(read, [](file_t &f, const cs::numeric &size,
		const cs::numeric &deadline_ms) -> cs::var {
			if (!f || f->closed || !f->readable) return cs::null_pointer;
			const int sz = static_cast<int>(size.as_integer());
			if (sz <= 0) return cs::var::make<std::string>(std::string{});
			const auto dl = deadline_ms.as_integer();
			const bool infinite = (dl < 0);
			const auto end = std::chrono::steady_clock::now()
			                 + std::chrono::milliseconds(infinite ? 0 : dl);
			std::vector<char> buf(sz);
			while (true) {
				int n = f->read_at(buf.data(), sz);
				if (n > 0) {
					f->read_pos += n;
					return cs::var::make<std::string>(std::string(buf.data(), n));
				}
				if (n < 0) return cs::null_pointer;
				// n == 0: EOF
				return cs::var::make<std::string>(std::string{});
			}
		})
		// write(data, deadline_ms): write data to the file.
		// Returns bytes written or -1 on error.
		CNI_V(write, [](file_t &f, const std::string &data,
		const cs::numeric & /*deadline_ms*/) -> int {
			if (!f || f->closed || !f->writable) return -1;
			return f->write_bytes(data.data(), static_cast<int>(data.size()));
		})
		// flush(deadline_ms): flush write buffers. Returns true on success.
		CNI_V(flush, [](file_t &f, const cs::numeric & /*deadline_ms*/) -> bool {
			if (!f || f->closed || !f->writable) return false;
			return f->flush_file();
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

		// Open or create a file at `path` with the given `mode`.
		// Modes: "r" (read), "w" (write/create/truncate), "a" (append),
		//        "r+" (read+write existing), "w+" (read+write/create/truncate)
		// Returns a file_t handle, or null on failure.
		CNI_V(fstream, [](const std::string &path, const std::string &mode) -> cs::var {
			auto f = mpp::open_file(path, mode);
			if (!f) return cs::null_pointer;
			return cs::var::make<file_t>(f);
		})

		// Restart the event loop after a stop() call.
		// libuv resumes automatically when uv_run() is called again; this is
		// a readability marker / future hook for pre-restart setup.
		CNI_V(restart, []() {
			// uv_run() can be called again after uv_stop() without extra setup.
		})
	}
	CNI_NAMESPACE_ALIAS(async, aio)

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
		// shell(string): enable shell mode with an explicit shell program.
		CNI_V(shell, [](builder_t &b, const std::string &program) {
			b.shell(program);
		})
		// Alias for script environments where member name `shell` is not reachable.
		CNI_V(use_shell, [](builder_t &b, const std::string &program) {
			b.shell(program);
		})
		// shell_off(): disable shell mode.
		CNI_V(shell_off, [](builder_t &b) {
			b.shell(nullptr);
		})
		// redirect_out(file_t): redirect child stdout to a file_t opened for writing.
		CNI_V(redirect_out, [](builder_t &b, const file_t &f) {
			if (!f || f->closed || !f->writable)
				mpp::throw_ex<mpp::runtime_error>("file_t is not open for writing");
			b.redirect_stdout(f->native_fd());
		})
		// clear_redirect_out(): clear previously configured stdout redirect.
		CNI_V(clear_redirect_out, [](builder_t &b) {
			b.redirect_stdout(mpp::FD_INVALID);
		})
		// redirect_err(file_t): redirect child stderr to a file_t opened for writing.
		CNI_V(redirect_err, [](builder_t &b, const file_t &f) {
			if (!f || f->closed || !f->writable)
				mpp::throw_ex<mpp::runtime_error>("file_t is not open for writing");
			b.redirect_stderr(f->native_fd());
		})
		// clear_redirect_err(): clear previously configured stderr redirect.
		CNI_V(clear_redirect_err, [](builder_t &b) {
			b.redirect_stderr(mpp::FD_INVALID);
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
			// In a fiber context: launch begin_wait() so a background OS thread does
			// the single blocking waitpid/WaitForSingleObject, then cooperatively
			// yield until poll_wait() sees the future ready — zero syscalls per tick.
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
			// poll_wait() checks the future state without a syscall (if begin_wait()
			// was already called), or falls back to a single process_exited() check.
			if (p->poll_wait())
			{
				return cs::var::make<cs::numeric>(p->collect_wait());
			}
			return cs::null_pointer;
		})
		CNI_V(wait_poll, [](const process_t &p, const cs::numeric &timeout_ms,
		const cs::numeric &poll_interval_ms) -> cs::var {
			// Launch the async waiter once so every subsequent poll_wait() call is
			// a zero-syscall future status check (mutex only).
			p->begin_wait();
			const auto timeout = timeout_ms.as_integer();
			const int interval = std::max(1, static_cast<int>(poll_interval_ms.as_integer()));
			// Already done (e.g. second call after process exited).
			if (p->poll_wait())
			{
				return cs::var::make<cs::numeric>(p->collect_wait());
			}
			if (timeout < 0)
			{
				// Indefinite: poll until the future is ready.
				while (!p->poll_wait())
					cs_runtime_yield(interval);
				return cs::var::make<cs::numeric>(p->collect_wait());
			}
			const auto deadline = std::chrono::steady_clock::now()
			                      + std::chrono::milliseconds(timeout);
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
		CNI_V(wait_with, [](const process_t &p, const cs::numeric &timeout_ms,
		const cs::var &on_wait) -> cs::var {
			p->begin_wait();
			const auto timeout = timeout_ms.as_integer();
			if (p->poll_wait())
			{
				return cs::var::make<cs::numeric>(p->collect_wait());
			}
			if (timeout < 0)
			{
				// Indefinite: drive the callback loop until the future is ready.
				while (!p->poll_wait())
					cs::invoke(on_wait);
				return cs::var::make<cs::numeric>(p->collect_wait());
			}
			const auto deadline = std::chrono::steady_clock::now()
			                      + std::chrono::milliseconds(timeout);
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
		// poll() -> bool: non-blocking exit check (API §2.5).
		CNI_V(poll, [](const process_t &p) -> bool {
			return p->has_exited();
		})
		// wait_for(ms) -> bool: wait up to ms milliseconds; true if exited.
		CNI_V(wait_for, [](const process_t &p, const cs::numeric &ms) -> bool {
			p->begin_wait();
			if (p->poll_wait()) return true;
			const auto deadline = std::chrono::steady_clock::now()
			                      + std::chrono::milliseconds(ms.as_integer());
			while (std::chrono::steady_clock::now() < deadline) {
				cs_runtime_yield(5);
				if (p->poll_wait()) return true;
			}
			return false;
		})
		// wait_until(deadline_ms) -> bool: wait until system-clock ms timestamp.
		// Use runtime.time() + offset to compute the deadline on the script side.
		CNI_V(wait_until, [](const process_t &p, const cs::numeric &deadline_ms) -> bool {
			p->begin_wait();
			if (p->poll_wait()) return true;
			const auto deadline = std::chrono::time_point<std::chrono::system_clock>(
			    std::chrono::milliseconds(deadline_ms.as_integer()));
			while (std::chrono::system_clock::now() < deadline) {
				cs_runtime_yield(5);
				if (p->poll_wait()) return true;
			}
			return false;
		})
		CNI_V(kill, [](const process_t &p, bool force) {
			p->interrupt(force);
		})
		CNI_V(get_pid, [](const process_t &p) {
			return p->pid();
		})
		CNI_V(communicate, [](const process_t &p) {
			// Drains stdout and stderr simultaneously to avoid pipe-full deadlocks,
			// waits for the process to exit, and returns {stdout, stderr, exit_code}.
			// In a fiber context: begin_communicate() launches the reader futures
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