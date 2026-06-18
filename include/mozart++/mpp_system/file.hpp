/**
 * Mozart++ Template Library: System/File — forked from
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

#ifdef MOZART_PLATFORM_WIN32
// Windows.h is already pulled in by mozart++/core → mpp_foundation/io.hpp
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

#include <memory>
#include <string>

namespace mpp {

/**
 * Synchronous, seekable, native-OS file wrapper used as the mpp-layer
 * counterpart of the CovScript `file_t` type.
 *
 * - Wraps a Win32 HANDLE or a POSIX fd.
 * - Handle is opened with the inheritable flag so it can be used directly
 *   as a process redirect target via process_builder::redirect_stdout/stderr.
 * - Lazily creates fdistream / fdostream wrappers on first access via out()/in().
 * - close() is idempotent; the destructor calls it automatically.
 */
class file {
public:
	bool readable  = false;
	bool writable  = false;
	bool closed    = false;
	int64_t read_pos = 0;

private:
	std::unique_ptr<fdistream> _istream;
	std::unique_ptr<fdostream> _ostream;

public:
#ifdef MOZART_PLATFORM_WIN32
	HANDLE handle = INVALID_HANDLE_VALUE;

	fd_type native_fd() const { return handle; }

	void close_file()
	{
		if (closed) return;
		closed = true;
		_istream.reset();
		_ostream.reset();
		if (handle != INVALID_HANDLE_VALUE) {
			CloseHandle(handle);
			handle = INVALID_HANDLE_VALUE;
		}
	}

	/**
	 * Read up to size bytes at the current read_pos.
	 * Returns bytes read (> 0), 0 on EOF, -1 on error.
	 */
	int read_at(char *buf, int size)
	{
		if (!readable || closed || handle == INVALID_HANDLE_VALUE) return -1;
		LARGE_INTEGER pos;
		pos.QuadPart = read_pos;
		if (!SetFilePointerEx(handle, pos, nullptr, FILE_BEGIN)) return -1;
		DWORD n = 0;
		if (!ReadFile(handle, buf, static_cast<DWORD>(size), &n, nullptr)) {
			if (GetLastError() == ERROR_HANDLE_EOF) return 0;
			return -1;
		}
		return static_cast<int>(n);
	}

	int write_bytes(const char *buf, int size)
	{
		if (!writable || closed || handle == INVALID_HANDLE_VALUE) return -1;
		DWORD n = 0;
		if (!WriteFile(handle, buf, static_cast<DWORD>(size), &n, nullptr)) return -1;
		return static_cast<int>(n);
	}

	bool flush_file()
	{
		if (!writable || closed || handle == INVALID_HANDLE_VALUE) return false;
		return FlushFileBuffers(handle) != FALSE;
	}

#else
	int fd = -1;

	fd_type native_fd() const { return fd; }

	void close_file()
	{
		if (closed) return;
		closed = true;
		_istream.reset();
		_ostream.reset();
		if (fd >= 0) {
			::close(fd);
			fd = -1;
		}
	}

	/**
	 * Read up to size bytes at the current read_pos.
	 * Returns bytes read (> 0), 0 on EOF, -1 on error.
	 */
	int read_at(char *buf, int size)
	{
		if (!readable || closed || fd < 0) return -1;
		ssize_t n = ::pread(fd, buf, static_cast<size_t>(size), static_cast<off_t>(read_pos));
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
			return -1;
		}
		return static_cast<int>(n);
	}

	int write_bytes(const char *buf, int size)
	{
		if (!writable || closed || fd < 0) return -1;
		ssize_t n = ::write(fd, buf, static_cast<size_t>(size));
		return static_cast<int>(n);
	}

	bool flush_file()
	{
		if (!writable || closed || fd < 0) return false;
		return ::fsync(fd) == 0;
	}
#endif

	/**
	 * Stream wrapper for reading file content (for cs::istream binding).
	 * The wrapper is owned by this file object and is invalidated by close().
	 */
	fdistream &out_stream()
	{
		if (!_istream) _istream = std::make_unique<fdistream>(native_fd());
		return *_istream;
	}

	/**
	 * Stream wrapper for writing to the file (for cs::ostream binding).
	 */
	fdostream &in_stream()
	{
		if (!_ostream) _ostream = std::make_unique<fdostream>(native_fd());
		return *_ostream;
	}

	~file() { close_file(); }
};

using file_ptr = std::shared_ptr<file>;

/**
 * Open or create a file at `path` with the given `mode` string.
 *
 * Supported modes:
 *   "r"   — read-only, file must exist
 *   "w"   — write-only, create/truncate
 *   "a"   — append-only, create if absent
 *   "r+"  — read+write, file must exist
 *   "w+"  — read+write, create/truncate
 *
 * Returns a valid file_ptr on success, an empty file_ptr (nullptr) on failure.
 * Throws mpp::runtime_error for unrecognised mode strings.
 */
inline file_ptr open_file(const std::string &path, const std::string &mode)
{
	const bool read_only   = (mode == "r");
	const bool write_only  = (mode == "w" || mode == "a");
	const bool read_write  = (mode == "r+" || mode == "w+");
	const bool append_mode = (mode == "a");
	const bool create_trunc = (mode == "w" || mode == "w+");

	if (!read_only && !write_only && !read_write)
		mpp::throw_ex<mpp::runtime_error>("unsupported file mode: " + mode);

	auto f = std::make_shared<mpp::file>();
	f->readable = read_only || read_write;
	f->writable = write_only || read_write;

#ifdef MOZART_PLATFORM_WIN32
	DWORD access = 0;
	if (f->readable) access |= GENERIC_READ;
	if (f->writable) access |= GENERIC_WRITE;

	DWORD disposition = OPEN_EXISTING;
	if (create_trunc)     disposition = CREATE_ALWAYS;
	else if (append_mode) disposition = OPEN_ALWAYS;

	SECURITY_ATTRIBUTES sa = {};
	sa.nLength        = sizeof(sa);
	sa.bInheritHandle = TRUE; // required for process redirect inheritance

	f->handle = CreateFileA(
	    path.c_str(), access,
	    FILE_SHARE_READ | FILE_SHARE_WRITE,
	    &sa, disposition,
	    FILE_ATTRIBUTE_NORMAL, nullptr);

	if (f->handle == INVALID_HANDLE_VALUE)
		return {};

	if (append_mode) {
		LARGE_INTEGER zero = {};
		SetFilePointerEx(f->handle, zero, nullptr, FILE_END);
	}
#else
	int flags = 0;
	if (read_only)
		flags = O_RDONLY;
	else if (write_only)
		flags = O_WRONLY | (create_trunc ? O_CREAT | O_TRUNC : O_CREAT | O_APPEND);
	else /* read_write */
		flags = O_RDWR | (create_trunc ? O_CREAT | O_TRUNC : 0);

	f->fd = ::open(path.c_str(), flags, 0666);
	if (f->fd < 0) return {};
#endif

	return f;
}

} // namespace mpp
