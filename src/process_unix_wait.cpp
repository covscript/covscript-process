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

#ifdef MOZART_PLATFORM_UNIX

#include <mozart++/process>

#include <cerrno>
#include <csignal>
#include <ctime>
#include <optional>
#include <sys/stat.h>
#include <sys/wait.h>

namespace mpp_impl {
	/**
	 * Poll child process status without reaping the exit value.
	 *
	 * Returns the exit code if the process has exited, or std::nullopt if
	 * the process is still running or an error occurred. When std::nullopt
	 * is returned and errno == ECHILD, the process has already been reaped
	 * by another waiter (e.g. a SIGCHLD handler). Otherwise errno is zero
	 * and the process is simply still alive.
	 *
	 * Only waits for WEXITED — a stopped (SIGSTOP/SIGTSTP) process is still
	 * alive and should not be treated as exited.
	 */
	static std::optional<int> poll_process_status(int pid)
	{
		siginfo_t info;
		memset(&info, '\0', sizeof(info));
		errno = 0;
		if (waitid(P_PID, pid, &info, WEXITED | WNOHANG | WNOWAIT) == -1) {
			return std::nullopt;
		}

		switch (info.si_code) {
		case CLD_EXITED:
			return info.si_status;
		case CLD_KILLED:
		case CLD_DUMPED:
			return 0x80 + WTERMSIG(info.si_status);
		default:
			return std::nullopt;
		}
	}

	int wait_for(const process_info &info)
	{
		// Block until the child exits, instead of polling with sched_yield().
		// We deliberately do NOT pass WNOWAIT here: this call also reaps the
		// zombie process, which previously was never collected (close_process()
		// only closes file descriptors). After reaping, subsequent calls to
		// process_exited()/poll_process_status() will get ECHILD; the existing
		// branches there already treat ECHILD as "process is gone" and return
		// the cached exit code held in mpp::process::_exit_code.
		//
		// WSTOPPED is intentionally omitted: a stopped child (SIGSTOP/SIGTSTP)
		// is still alive and should not cause wait_for() to return.
		while (true) {
			siginfo_t si;
			memset(&si, '\0', sizeof(si));
			if (waitid(P_PID, info._pid, &si, WEXITED) == -1) {
				if (errno == EINTR) {
					continue;
				}
				if (errno == ECHILD) {
					return 0;
				}
				return -1;
			}
			switch (si.si_code) {
			case CLD_EXITED:
				return si.si_status;
			case CLD_KILLED:
			case CLD_DUMPED:
				return 0x80 + WTERMSIG(si.si_status);
			default:
				// Spurious wakeup or unexpected si_code; loop and wait again.
				continue;
			}
		}
	}

	void terminate_process(const process_info &info, bool force)
	{
		kill(info._pid, force ? SIGKILL : SIGTERM);
	}

	void terminate_process_tree(const process_info &info, bool force)
	{
		const int sig = force ? SIGKILL : SIGTERM;
		// Child processes are launched into their own process group (PGID=PID),
		// so negative PID targets the whole subtree group.
		if (info._pid > 0)
			kill(-info._pid, sig);
	}

	bool wait_timeout_ms(const process_info &info, int timeout_ms, int &exit_code,
	                     int poll_interval_ms)
	{
		const int64_t poll_ns = static_cast<int64_t>(std::max(1, poll_interval_ms)) * 1000000LL;
		int64_t remaining_ns = static_cast<int64_t>(timeout_ms) * 1000000LL;
		struct timespec ts = {
			static_cast<time_t>(poll_ns / 1000000000LL),
			static_cast<long>(poll_ns % 1000000000LL)
		};
		while (remaining_ns > 0) {
			auto status = poll_process_status(info._pid);
			if (status.has_value()) {
				exit_code = status.value();
				// poll_process_status() uses WNOWAIT; reap the zombie here
				siginfo_t si;
				waitid(P_PID, info._pid, &si, WEXITED | WNOHANG);
				return true;
			}
			if (errno == ECHILD) {
				exit_code = 0;
				return true;
			}
			// Handle EINTR so a signal doesn't shorten the wait.
			struct timespec rem = {0, 0};
			if (nanosleep(&ts, &rem) == -1 && errno == EINTR) {
				int64_t intr_ns = static_cast<int64_t>(rem.tv_sec) * 1000000000LL + rem.tv_nsec;
				remaining_ns -= (poll_ns - intr_ns);
			}
			else {
				remaining_ns -= poll_ns;
			}
		}

		auto status = poll_process_status(info._pid);
		if (status.has_value()) {
			exit_code = status.value();
			// poll_process_status() uses WNOWAIT; reap the zombie here
			siginfo_t si;
			waitid(P_PID, info._pid, &si, WEXITED | WNOHANG);
			return true;
		}
		if (errno == ECHILD) {
			exit_code = 0;
			return true;
		}
		return false;
	}

	bool process_exited(const process_info &info)
	{
		auto status = poll_process_status(info._pid);

		if (status.has_value()) {
			return true;
		}

		if (errno == ECHILD) {
			// Process was already reaped (e.g. by a SIGCHLD handler).
			struct sigaction sa {};
			if (sigaction(SIGCHLD, nullptr, &sa) != 0) {
				mpp::throw_ex<mpp::runtime_error>("should not reach here");
			}

#if defined(MOZART_PLATFORM_DARWIN)
			void *handler = reinterpret_cast<void *>(sa.__sigaction_u.__sa_handler);
#else
			void *handler = reinterpret_cast<void *>(sa.sa_handler);
#endif

			if (handler == reinterpret_cast<void *>(SIG_IGN)) {
#if defined(MOZART_PLATFORM_DARWIN)
				return kill(info._pid, 0) == -1 && errno == ESRCH;
#else
				std::string path = std::string("/proc/") + std::to_string(info._pid);
				struct stat buf {};
				return stat(path.c_str(), &buf) == -1 && errno == ENOENT;
#endif
			}

			return true;
		}

		return false;
	}

	int get_pid(const process_info &info)
	{
		return static_cast<int>(info._pid);
	}
}

#endif
