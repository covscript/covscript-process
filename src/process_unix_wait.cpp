/**
 * Mozart++ Template Library
 * Licensed under Apache 2.0
 * Copyright (C) 2020-2021 Chengdu Covariant Technologies Co., LTD.
 * Website: https://covariant.cn/
 * Github:  https://github.com/chengdu-zhirui/
 */
#include <mozart++/core>

#ifdef MOZART_PLATFORM_UNIX

#include <mozart++/process>

#include <cerrno>
#include <sched.h>
#include <csignal>
#include <sys/stat.h>
#include <sys/wait.h>

namespace mpp_impl {
	/**
	 * We use -1 to indicate that a process is still running,
	 * because the return value of a process can never be -1.
	 */
	static constexpr int PROCESS_STILL_ALIVE = -1;
	static constexpr int PROCESS_POLL_FAILED = -2;

	/**
	 * Poll child process status without reaping the exit value.
	 */
	static int poll_process_status(int pid)
	{
		siginfo_t info;
		memset(&info, '\0', sizeof(info));

		if (waitid(P_PID, pid, &info, WEXITED | WSTOPPED | WNOHANG | WNOWAIT) == -1) {
			return PROCESS_POLL_FAILED;
		}

		switch (info.si_code) {
		case CLD_EXITED:
			return info.si_status;
		case CLD_KILLED:
		case CLD_DUMPED:
			return 0x80 + WTERMSIG(info.si_status);
		case CLD_STOPPED:
			return 0x80 + WSTOPSIG(info.si_status);
		default:
			return PROCESS_STILL_ALIVE;
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
		while (true) {
			siginfo_t si;
			memset(&si, '\0', sizeof(si));
			if (waitid(P_PID, info._pid, &si, WEXITED | WSTOPPED) == -1) {
				if (errno == EINTR) {
					continue;
				}
				if (errno == ECHILD) {
					// Already reaped (e.g. SIGCHLD ignored by the host process).
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
			case CLD_STOPPED:
				return 0x80 + WSTOPSIG(si.si_status);
			default:
				// Spurious wakeup; loop and wait again.
				continue;
			}
		}
	}

	void terminate_process(const process_info &info, bool force)
	{
		kill(info._pid, force ? SIGKILL : SIGTERM);
	}

	bool wait_timeout_ms(const process_info &info, int timeout_ms, int &exit_code,
	                     int poll_interval_ms)
	{
		const int64_t poll_ns = static_cast<int64_t>(std::max(1, poll_interval_ms)) * 1000000LL;
		int64_t remaining_ns = static_cast<int64_t>(timeout_ms) * 1000000LL;
		struct timespec ts = {0, static_cast<long>(poll_ns)};
		while (remaining_ns > 0) {
			int status = poll_process_status(info._pid);
			if (status != PROCESS_STILL_ALIVE) {
				if (status == PROCESS_POLL_FAILED) {
					if (errno == ECHILD) {
						exit_code = 0;
						return true;
					}
					return false;
				}
				exit_code = status;
				return true;
			}
			nanosleep(&ts, nullptr);
			remaining_ns -= poll_ns;
		}

		int status = poll_process_status(info._pid);
		if (status == PROCESS_POLL_FAILED) {
			if (errno == ECHILD) {
				exit_code = 0;
				return true;
			}
			return false;
		}
		if (status != PROCESS_STILL_ALIVE) {
			exit_code = status;
			return true;
		}
		return false;
	}

	bool process_exited(const process_info &info)
	{
		int status = poll_process_status(info._pid);

		if (status == PROCESS_POLL_FAILED) {
			if (errno != ECHILD) {
				mpp::throw_ex<mpp::runtime_error>("should not reach here");
			}

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

		return status != PROCESS_STILL_ALIVE;
	}

	int get_pid(const process_info &info)
	{
		return static_cast<int>(info._pid);
	}

	void send_signal(const process_info &info, int signum)
	{
		kill(static_cast<pid_t>(info._pid), signum);
	}
}

#endif
