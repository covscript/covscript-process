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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <optional>
#include <sys/stat.h>
#include <sys/wait.h>

#ifdef MOZART_PLATFORM_DARWIN
#include <sys/sysctl.h>
#endif

namespace mpp_impl {

	/**
	 * Read the start time of the process identified by @p pid.
	 * Returns 0 on failure (can't read, PID doesn't exist, etc.).
	 *
	 * Linux:   reads /proc/<pid>/stat field 22 (starttime in clock ticks).
	 * macOS:   uses sysctl KERN_PROC_PID to read p_starttime (sec+usec).
	 */
	uint64_t get_process_start_time(int pid)
	{
#ifdef MOZART_PLATFORM_DARWIN
		struct kinfo_proc kp;
		size_t len = sizeof(kp);
		int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
		if (sysctl(mib, 4, &kp, &len, nullptr, 0) != 0)
			return 0;
		const uint64_t sec = static_cast<uint64_t>(kp.kp_proc.p_starttime.tv_sec);
		const uint64_t usec = static_cast<uint64_t>(kp.kp_proc.p_starttime.tv_usec);
		return sec * 1000000ULL + usec;
#else
		// Linux: /proc/<pid>/stat field 22
		char path[64];
		snprintf(path, sizeof(path), "/proc/%d/stat", pid);
		FILE *fp = fopen(path, "r");
		if (!fp) return 0;

		// Read enough of the stat file to reach field 22
		char buf[1024];
		size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
		fclose(fp);
		if (n == 0) return 0;
		buf[n] = '\0';

		// The 'comm' field (field 2) may contain spaces and parentheses.
		// Find the closing ')' — everything after it is space-delimited.
		char *p = strrchr(buf, ')');
		if (!p) return 0;
		p += 2; // skip ") "

		// Fields after ')': state(3) ppid(4) pgrp(5) session(6) tty_nr(7)
		// tpgid(8) flags(9) minflt(10) cminflt(11) majflt(12) cmajflt(13)
		// utime(14) stime(15) cutime(16) cstime(17) priority(18) nice(19)
		// num_threads(20) itrealvalue(21) starttime(22)
		// We need to skip 19 fields to reach starttime.
		for (int i = 0; i < 19; ++i) {
			p = strchr(p, ' ');
			if (!p) return 0;
			++p;
		}
		return strtoull(p, nullptr, 10);
#endif
	}
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
			return 0x80 + info.si_status;
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
				return 0x80 + si.si_status;
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
		// Guard against PID reuse: if the process has already exited, the
		// PGID may now belong to an unrelated process.
		if (process_exited(info))
			return;
		const int sig = force ? SIGKILL : SIGTERM;

		// If we have a recorded start time, verify that the PID still
		// belongs to the same process before targeting its process group.
		// A mismatch means the PID was recycled — only kill the root PID,
		// not the process group (which now belongs to an unrelated tree).
		if (info._start_time > 0) {
			uint64_t current = get_process_start_time(info._pid);
			if (current == 0 || current != info._start_time) {
				// Identity uncertain or PID reused: degrade to root-only kill.
				if (info._pid > 0)
					kill(info._pid, sig);
				return;
			}
		}

		// Child processes are launched into their own process group (PGID=PID),
		// so negative PID targets the whole subtree group.
		if (info._pid > 0)
			kill(-info._pid, sig);
	}

	bool wait_timeout_ms(const process_info &info, int timeout_ms, int &exit_code,
	                     int poll_interval_ms)
	{
		// Negative timeout: wait indefinitely (blocking wait_for).
		if (timeout_ms < 0) {
			exit_code = wait_for(info);
			return true;
		}

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
			// POSIX.1-2001 guarantees that converting between function pointers
			// and void* works for SIG_DFL / SIG_IGN comparisons (technically UB
			// under the C++ standard, but safe on all supported targets where
			// sizeof(void*) >= sizeof(sa_handler)).
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
				// With SIG_IGN the child is auto-reaped on exit, so ECHILD
				// from waitid means the child IS gone.  However its PID may
				// have been recycled by an unrelated process.  If we
				// recorded a start time we can verify whether the current
				// occupant is still the original child.
				if (info._start_time > 0) {
					uint64_t current = get_process_start_time(info._pid);
					// current == 0 means "can't read" → degrade safely.
					// current != info._start_time → PID reused → child gone.
					if (current == 0 || current != info._start_time)
						return true;
					// Start time matches but we got ECHILD — contradictory,
					// but ECHILD is authoritative: the kernel says our child
					// no longer exists as our child.
					return true;
				}
				// No start-time record: degrade safely.  ECHILD is
				// authoritative — the child no longer exists.
				return true;
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
