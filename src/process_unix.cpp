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
#include <mozart++/string>
#include <dirent.h>
#include <cerrno>
#include <fcntl.h>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <cctype>
#include <climits>
#include <limits>
#include <sys/wait.h>

#ifdef __linux__
#include <sys/syscall.h>
#endif

#ifdef MOZART_PLATFORM_DARWIN
#define FD_DIR "/dev/fd"
#endif

#ifdef MOZART_PLATFORM_DARWIN
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#else
extern char **environ;
#endif

namespace mpp_impl {
	/**
	 * Close all file descriptors >= from_fd, except fail_fd.
	 *
	 * Strategy varies by platform:
	 *   - Linux (kernel 5.9+): use close_range(2) syscall for O(1) close.
	 *   - macOS / Darwin: enumerate /dev/fd — reliable because opendir on
	 *     Darwin uses a stable internal fd that won't collide with the fds
	 *     we're closing.
	 *   - Fallback: iterate from from_fd to sysconf(_SC_OPEN_MAX).
	 *
	 * The old opendir("/proc/self/fd") approach on Linux assumed that opendir
	 * uses the lowest available fd.  While usually true, this is not a POSIX
	 * guarantee; if the assumption is wrong, readdir's internal fd could be
	 * closed mid-iteration, causing undefined behavior.  The strategies above
	 * avoid this class of bug entirely.
	 */
	static void close_all_descriptors(int from_fd, int fail_fd)
	{
#if defined(__linux__) && defined(SYS_close_range)
		// close_range(2) — Linux 5.9+.  Close [from_fd, UINT_MAX] then
		// re-open fail_fd if it was inadvertently closed (unlikely since
		// fail_fd is typically < from_fd, but be safe).
		unsigned int first = static_cast<unsigned int>(from_fd);
		unsigned int last = std::numeric_limits<unsigned int>::max();
		if (syscall(SYS_close_range, first, last, 0) == 0) {
			// close_range succeeded.  It may have closed fail_fd if
			// fail_fd >= from_fd, but in practice fail_fd is always
			// less than from_fd (it's the read end of the fail pipe,
			// opened before any std fds are duped).
			return;
		}
		// Fall through to enumeration on EINVAL/ENOSYS (old kernel).
#endif

#ifdef MOZART_PLATFORM_DARWIN
		// macOS: /dev/fd is reliable — opendir's fd is tracked separately.
		DIR *dp = opendir(FD_DIR);
		if (dp != nullptr) {
			struct dirent *entry;
			while ((entry = readdir(dp)) != nullptr) {
				int fd;
				if (std::isdigit(entry->d_name[0])
				        && (fd = strtol(entry->d_name, nullptr, 10)) >= from_fd
				        && fd != fail_fd) {
					close(fd);
				}
			}
			closedir(dp);
			return;
		}
#endif

		// Generic fallback: brute-force iterate up to the fd limit.
		int max_fd = static_cast<int>(sysconf(_SC_OPEN_MAX));
		if (max_fd < 0) max_fd = 1024; // conservative default
		for (int fd = from_fd; fd < max_fd; fd++) {
			if (fd == fail_fd) continue;
			close(fd); // ignore EBADF
		}
	}

	/*
	 * Reads nbyte bytes from file descriptor fd into buf,
	 * The read operation is retried in case of EINTR or partial reads.
	 *
	 * Returns number of bytes read (normally nbyte, but may be less in
	 * case of EOF).  In case of read errors, returns -1 and sets errno.
	 */
	mpp::ssize_t read_fully(int fd, void *buf, size_t nbyte)
	{
		ssize_t remaining = nbyte;
		while (true) {
			ssize_t n = read(fd, buf, remaining);
			if (n == 0) {
				return nbyte - remaining;
			}
			else if (n > 0) {
				remaining -= n;
				if (remaining <= 0) {
					return nbyte;
				}
				// We were interrupted in the middle of reading the bytes.
				// Unlikely, but possible.
				buf = reinterpret_cast<void *>(reinterpret_cast<char *>(buf) + n);
			}
			else if (errno == EINTR) {
				// we received some strange signals, which interrupted the
				// read system call, we just proceed to continue reading.
			}
			else {
				return -1;
			}
		}
	}

	/**
	 * If PATH is not defined, the OS provides some default value.
	 */
	static const char *default_path_env()
	{
		return ":/bin:/usr/bin";
	}

	static const char *get_path_env()
	{
		const char *s = getenv("PATH");
		return (s != nullptr) ? s : default_path_env();
	}

	static const char *const *effective_pathv()
	{
		const char *path = get_path_env();
		// it's safe to convert from size_t to int, :)
		int count = static_cast<int>(mpp::string_ref(path).count(':')) + 1;
		size_t pathvsize = sizeof(const char *) * (count + 1);
		size_t pathsize = strlen(path) + 1;
		const char **pathv = reinterpret_cast<const char **>(malloc(pathvsize + pathsize));

		if (pathv == nullptr) {
			return nullptr;
		}

		char *p = reinterpret_cast<char *>(pathv + count + 1);
		memcpy(p, path, pathsize);

		// split PATH by replacing ':' with '\0'
		// and empty components with "."
		for (int i = 0; i < count; i++) {
			char *sep = p + strcspn(p, ":");
			pathv[i] = (p == sep) ? "." : p;
			if (*sep == ':') {
				*sep = '\0';
			}
			p = sep + 1;
		}
		pathv[count] = nullptr;
		return pathv;
	}

	/**
	 * Exec file as a shell script but without shebang (#!).
	 * This is a historical tradeoff.
	 * see GNU libc documentation.
	 */
	static void execve_without_shebang(const char *file, const char **argv, char **envp)
	{
		// Use the extra word of space provided for us in argv by caller.
		const char *argv0 = argv[0];
		const char *const *end = argv;
		while (*end != nullptr) {
			++end;
		}
		memmove(argv + 2, argv + 1, (end - argv) * sizeof(*end));
		argv[0] = "/bin/sh";
		argv[1] = file;
		execve(argv[0], const_cast<char **>(argv), envp);

		// oops, /bin/sh can't be executed, just fall through
		memmove(argv + 1, argv + 2, (end - argv) * sizeof(*end));
		argv[0] = argv0;
	}

	/**
	 * Like execve(2), but the file is always assumed to be a shell script
	 * and the system default shell is invoked to run it.
	 */
	static void execve_or_shebang(const char *file, const char **argv, char **envp)
	{
		execve(file, const_cast<char **>(argv), envp);
		// or the shell doesn't provide a shebang
		if (errno == ENOEXEC) {
			execve_without_shebang(file, argv, envp);
		}
	}

	/**
	 * mpp implementation of the GNU extension execvpe()
	 */
	static void mpp_execvpe(const char *file, const char **argv, char **envp)
	{
		if (envp == nullptr || envp == environ) {
			execvp(file, const_cast<char *const *>(argv));
			return;
		}

		if (*file == '\0') {
			errno = ENOENT;
			return;
		}

		if (strchr(file, '/') != nullptr) {
			execve_or_shebang(file, argv, envp);

		}
		else {
			// We must search PATH (parent's, not child's)
			const char *const *pathv = effective_pathv();

			if (pathv == nullptr) {
				errno = ENOMEM;
				return;
			}

			// prepare the full space to avoid memory allocation
			char absolute_path[PATH_MAX] = {0};
			int filelen = strlen(file);
			int sticky_errno = 0;

			for (auto dirs = pathv; *dirs; dirs++) {
				const char *dir = *dirs;
				int dirlen = strlen(dir);
				if (filelen + dirlen + 2 >= PATH_MAX) {
					errno = ENAMETOOLONG;
					continue;
				}

				memcpy(absolute_path, dir, dirlen);
				if (absolute_path[dirlen - 1] != '/') {
					absolute_path[dirlen++] = '/';
				}

				memcpy(absolute_path + dirlen, file, filelen);
				absolute_path[dirlen + filelen] = '\0';
				execve_or_shebang(absolute_path, argv, envp);

				// If permission is denied for a file (the attempted
				// execve returned EACCES), these functions will continue
				// searching the rest of the search path.  If no other
				// file is found, however, they will return with the
				// global variable errno set to EACCES.
				switch (errno) {
				case EACCES:
					sticky_errno = errno;
				// fall-through
				case ENOENT:
				case ENOTDIR:
#ifdef ELOOP
				case ELOOP:
#endif
#ifdef ESTALE
				case ESTALE:
#endif
#ifdef ENODEV
				case ENODEV:
#endif
#ifdef ETIMEDOUT
				case ETIMEDOUT:
#endif
					// Try other directories in PATH
					break;
				default:
					free((void*)pathv);
					return;
				}
			}
			free((void*)pathv);

			// tell the caller the real errno
			if (sticky_errno != 0) {
				errno = sticky_errno;
			}
		}
	}

	static void restartable_write_error(int fail_fd, int errnum)
	{
		ssize_t result = 0;
		do {
			result = write(fail_fd, &errnum, sizeof(errnum));
		}
		while ((result == -1) && (errno == EINTR));
	}

	__attribute__((noreturn))
	static void exit_with_error(int fail_fd)
	{
		// the child failed to exec, tell our parent.
		int errnum = errno;
		restartable_write_error(fail_fd, errnum);
		close(fail_fd);
		_exit(-1);
	}

	__attribute__((noreturn))
	static void child_proc(const process_startup &startup, process_info &info,
	                       fd_type *pstdin, fd_type *pstdout, fd_type *pstderr,
	                       fd_type *pfail, char **prebuilt_envp)
	{
		// Put child in a dedicated process group for kill-tree semantics.
		setpgid(0, 0);

		// close child side of read pipe
		close_fd(pfail[PIPE_READ]);
		int fail_fd = pfail[PIPE_WRITE];

		// Close ends the child doesn't need (for inherited streams these are FD_INVALID → no-op)
		if (!startup.inherit_stdin && !startup._stdin.redirected()) {
			close_fd(pstdin[PIPE_WRITE]);
		}
		if (!startup.inherit_stdout && !startup._stdout.redirected()) {
			close_fd(pstdout[PIPE_READ]);
		}

		// Set up stdin
		if (!startup.inherit_stdin) {
			dup2(pstdin[PIPE_READ], STDIN_FILENO);
		}

		// Set up stdout
		if (!startup.inherit_stdout) {
			dup2(pstdout[PIPE_WRITE], STDOUT_FILENO);
		}

		/*
		 * pay special attention to stderr:
		 *   1. merge: redirect stderr to wherever stdout ended up
		 *   2. inherit_stderr: leave as-is
		 *   3. normal: connect to its own pipe
		 */
		if (startup.merge_outputs) {
			// STDOUT_FILENO is already set up (either inherited or duped)
			dup2(STDOUT_FILENO, STDERR_FILENO);
		}
		else if (!startup.inherit_stderr) {
			if (!startup._stderr.redirected()) {
				close_fd(pstderr[PIPE_READ]);
			}
			dup2(pstderr[PIPE_WRITE], STDERR_FILENO);
		}
		// if inherit_stderr: leave STDERR_FILENO pointing at parent's stderr

		if (!startup.inherit_stdin)  close_fd(pstdin[PIPE_READ]);
		if (!startup.inherit_stdout) close_fd(pstdout[PIPE_WRITE]);
		if (!startup.inherit_stderr && !startup.merge_outputs) close_fd(pstderr[PIPE_WRITE]);

		// command-line and environments
		size_t asize = startup._cmdline.size();
		char *argv[asize + 1];

		// argv is always terminated with a nullptr
		argv[asize] = nullptr;

		// copy command-line arguments (points into startup._cmdline, valid after fork)
		for (std::size_t i = 0; i < asize; ++i) {
			argv[i] = const_cast<char *>(startup._cmdline[i].c_str());
		}

		// prebuilt_envp was constructed by the parent before fork, no heap allocation needed here.

		// close everything above stderr
		close_all_descriptors(STDERR_FILENO + 1, fail_fd);

		// change cwd
		if (chdir(startup._cwd.c_str()) != 0) {
			exit_with_error(fail_fd);
			// never return
		}

		// make 100% sure the fail pipe will be closed,
		// or the parent may get stuck in read_fully.
		if (fcntl(fail_fd, F_SETFD, FD_CLOEXEC) == -1) {
			// oops, we lost our double-insurance
			exit_with_error(fail_fd);
			// never return
		}

		// run subprocess
		mpp_execvpe(argv[0], const_cast<const char **>(argv), prebuilt_envp);

		// exec failed
		exit_with_error(fail_fd);
		// never return
	}

	void create_process_impl(const process_startup &startup, process_info &info,
	                         fd_type *pstdin, fd_type *pstdout, fd_type *pstderr)
	{
		// Build envp strings BEFORE fork to avoid heap allocation in the child
		// process, where the allocator may be in an inconsistent state if the
		// parent is multi-threaded.
		std::vector<std::string> env_strings;
		std::vector<char *> envp_vec;
		char **prebuilt_envp_ptr;

		if (startup._inherit_env && startup._env.empty()) {
			// nullptr → mpp_execvpe calls execvp which inherits the parent's
			// full environment without any copying.
			prebuilt_envp_ptr = nullptr;
		} else {
			if (startup._inherit_env) {
				// Merge: start from parent environ, then apply _env overrides.
				// All string construction happens before fork (no heap in child).
				std::unordered_map<std::string, std::string> merged;
				for (char **ep = environ; ep && *ep; ++ep) {
					std::string entry(*ep);
					auto eq = entry.find('=');
					if (eq != std::string::npos)
						merged.emplace(entry.substr(0, eq), entry.substr(eq + 1));
				}
				for (const auto &e : startup._env)
					merged[e.first] = e.second;
				env_strings.reserve(merged.size());
				for (const auto &e : merged)
					env_strings.push_back(e.first + "=" + e.second);
			} else {
				// inherit_env=false: only _env (empty env block if _env is also empty).
				env_strings.reserve(startup._env.size());
				for (const auto &e : startup._env)
					env_strings.push_back(e.first + "=" + e.second);
			}
			envp_vec.reserve(env_strings.size() + 1);
			for (auto &s : env_strings)
				envp_vec.push_back(const_cast<char *>(s.c_str()));
			envp_vec.push_back(nullptr);
			prebuilt_envp_ptr = envp_vec.data();
		}

		// the child_proc will use this pipe to
		// tell parent whether the process has started.
		fd_type pfail[2] = {FD_INVALID, FD_INVALID};
		if (!create_pipe(pfail)) {
			mpp::throw_ex<mpp::runtime_error>("unable to create communication pipe");
		}

		pid_t pid = fork();

		if (pid < 0) {
			close_pipe(pfail);
			mpp::throw_ex<mpp::runtime_error>("unable to fork subprocess");

		}
		else if (pid == 0) {
			// in child process, pfail will be closed in child_proc
			child_proc(startup, info, pstdin, pstdout, pstderr, pfail, prebuilt_envp_ptr);

			// child never returns

		}
		else {
			// in parent process

			// Best-effort reinforcement of child's process-group leader role.
			setpgid(pid, pid);

			// receive exec call result form child
			close_fd(pfail[PIPE_WRITE]);
			int child_errno = 0;

			switch (read_fully(pfail[PIPE_READ], &child_errno, sizeof(child_errno))) {
			case 0:
				// child exec succeeded.
				break;
			case sizeof(child_errno):
				// child failed to exec, we will wait it.
				waitpid(pid, nullptr, 0);
				close_fd(pfail[PIPE_READ]);
				mpp::throw_ex<mpp::runtime_error>("child exec failed: " + std::string(strerror(child_errno)));
				break;
			default:
				close_fd(pfail[PIPE_READ]);
				mpp::throw_ex<mpp::runtime_error>("read failed: " + std::string(strerror(errno)));
				break;
			}

			close_fd(pfail[PIPE_READ]);

			if (!startup.inherit_stdin && !startup._stdin.redirected()) {
				close_fd(pstdin[PIPE_READ]);
			}
			if (!startup.inherit_stdout && !startup._stdout.redirected()) {
				close_fd(pstdout[PIPE_WRITE]);
			}

			/*
			 * pay special attention to stderr,
			 * there are 2 cases:
			 *      1. redirect stderr to stdout
			 *      2. redirect stderr to a file
			 */
			if (startup.merge_outputs || startup.inherit_stdout || startup.inherit_stderr) {
				// nothing to close on parent side
			}
			else {
				// redirect stderr to a file
				if (!startup._stderr.redirected()) {
					close_fd(pstderr[PIPE_WRITE]);
				}
			}

			info._pid = pid;
			// Only store pipe fds that we own.  Redirect targets and inherited
			// streams are owned by the caller (file_t / OS), so we must not
			// close them in close_process().
			info._stdin  = (startup.inherit_stdin  || startup._stdin.redirected())
			               ? FD_INVALID : pstdin[PIPE_WRITE];
			info._stdout = (startup.inherit_stdout || startup._stdout.redirected())
			               ? FD_INVALID : pstdout[PIPE_READ];
			info._stderr = (startup.merge_outputs || startup.inherit_stdout
			                || startup.inherit_stderr || startup._stderr.redirected())
			               ? FD_INVALID : pstderr[PIPE_READ];

			// on *nix systems, fork() doesn't create threads to run process
			info._tid = FD_INVALID;
		}
	}

	void close_process(process_info &info)
	{
		mpp_impl::close_fd(info._stdin);
		mpp_impl::close_fd(info._stdout);
		mpp_impl::close_fd(info._stderr);
	}

}

#endif
