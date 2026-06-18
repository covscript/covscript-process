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
 *
 * Thread safety: not thread-safe. Concurrent reads/writes on the same file
 * object require external synchronization.
 */
class file {
private:
	bool _readable  = false;
	bool _writable  = false;
	bool _closed    = false;
	bool _append    = false;
	int64_t _read_pos  = 0;
	int64_t _write_pos = 0;

	std::unique_ptr<fdistream> _istream;
	std::unique_ptr<fdostream> _ostream;

#ifdef MOZART_PLATFORM_WIN32
	HANDLE _handle = INVALID_HANDLE_VALUE;
	int _uv_fd = -1;  // C runtime fd for libuv, created via _open_osfhandle
#else
	int _fd = -1;
#endif

public:
	file() = default;
	~file() { close_file(); }

	/**
	 * Configure initial state after opening.  Called by open_file().
	 * Not intended for general use.
	 */
	void configure(bool readable, bool writable, bool append)
	{
		_readable = readable;
		_writable = writable;
		_append = append;
	}

#ifdef MOZART_PLATFORM_WIN32
	void set_handle(HANDLE h) { _handle = h; }
	void set_uv_fd(int fd) { _uv_fd = fd; }
#else
	void set_fd(int fd) { _fd = fd; }
#endif

	// Non-copyable, non-movable (owns OS handle).
	file(const file &) = delete;
	file &operator=(const file &) = delete;
	file(file &&) = delete;
	file &operator=(file &&) = delete;

	// ------------------------------------------------------------------
	// State queries
	// ------------------------------------------------------------------

	/** True if the file is open for reading and not yet closed. */
	bool is_readable() const { return _readable && !_closed; }

	/** True if the file is open for writing and not yet closed. */
	bool is_writable() const { return _writable && !_closed; }

	/** True if close_file() has been called (or the destructor ran). */
	bool is_closed() const { return _closed; }

	/** True if the file was opened in append mode ("a"). */
	bool is_append() const { return _append; }

	// ------------------------------------------------------------------
	// Position tracking (used by CNI async read/write)
	// ------------------------------------------------------------------

	int64_t read_position() const { return _read_pos; }
	void advance_read(int64_t n) { _read_pos += n; }

	int64_t write_position() const { return _write_pos; }
	void advance_write(int64_t n) { _write_pos += n; }

	// ------------------------------------------------------------------
	// Native handle access
	// ------------------------------------------------------------------

#ifdef MOZART_PLATFORM_WIN32
	fd_type native_fd() const { return _handle; }
	int get_uv_fd() const { return _uv_fd; }
#else
	fd_type native_fd() const { return _fd; }
#endif

	// ------------------------------------------------------------------
	// Lifecycle
	// ------------------------------------------------------------------

	/**
	 * Close the file.  Idempotent — second and subsequent calls are no-ops.
	 * The destructor calls this automatically.
	 */
	void close_file()
	{
		if (_closed) return;
		_closed = true;
		_istream.reset();
		_ostream.reset();
#ifdef MOZART_PLATFORM_WIN32
		if (_uv_fd >= 0) {
			// _open_osfhandle transfers handle ownership to the C runtime fd.
			// Closing the fd also closes the underlying HANDLE.
			_close(_uv_fd);
			_uv_fd = -1;
			_handle = INVALID_HANDLE_VALUE;
		} else if (_handle != INVALID_HANDLE_VALUE) {
			CloseHandle(_handle);
			_handle = INVALID_HANDLE_VALUE;
		}
#else
		if (_fd >= 0) {
			::close(_fd);
			_fd = -1;
		}
#endif
	}

	// ------------------------------------------------------------------
	// Stream wrappers (lazy, for CNI cs::istream / cs::ostream binding)
	// ------------------------------------------------------------------

	/**
	 * Returns a readable stream wrapping the native fd.
	 * Used for reading file content (out() in CovScript).
	 * Returns a reference to an internally-owned fdistream; invalidated by close_file().
	 */
	fdistream &out_stream()
	{
		if (!_istream) _istream = std::make_unique<fdistream>(native_fd());
		return *_istream;
	}

	/**
	 * Returns a writable stream wrapping the native fd.
	 * Used for writing to the file (in() in CovScript).
	 * Returns a reference to an internally-owned fdostream; invalidated by close_file().
	 */
	fdostream &in_stream()
	{
		if (!_ostream) _ostream = std::make_unique<fdostream>(native_fd());
		return *_ostream;
	}
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
	f->configure(read_only || read_write, write_only || read_write, append_mode);

#ifdef MOZART_PLATFORM_WIN32
	DWORD access = 0;
	if (f->is_readable()) access |= GENERIC_READ;
	if (f->is_writable()) access |= GENERIC_WRITE;

	DWORD disposition = OPEN_EXISTING;
	if (create_trunc)     disposition = CREATE_ALWAYS;
	else if (append_mode) disposition = OPEN_ALWAYS;

	SECURITY_ATTRIBUTES sa = {};
	sa.nLength        = sizeof(sa);
	sa.bInheritHandle = TRUE; // required for process redirect inheritance

	HANDLE h = CreateFileA(
	    path.c_str(), access,
	    FILE_SHARE_READ | FILE_SHARE_WRITE,
	    &sa, disposition,
	    FILE_ATTRIBUTE_NORMAL, nullptr);

	if (h == INVALID_HANDLE_VALUE)
		return {};

	f->set_handle(h);

	// Create a C runtime fd for libuv async I/O once, so repeated
	// uv_fs_read / uv_fs_write calls share the same fd position.
	{
		int fl = 0;
		if (f->is_readable() && f->is_writable()) fl = _O_RDWR;
		else if (f->is_readable())                fl = _O_RDONLY;
		else                                      fl = _O_WRONLY;
		f->set_uv_fd(_open_osfhandle(reinterpret_cast<intptr_t>(h), fl));
	}

	if (append_mode) {
		LARGE_INTEGER zero = {};
		SetFilePointerEx(h, zero, nullptr, FILE_END);
	}
#else
	int flags = 0;
	if (read_only)
		flags = O_RDONLY;
	else if (write_only)
		flags = O_WRONLY | (create_trunc ? O_CREAT | O_TRUNC : O_CREAT | O_APPEND);
	else /* read_write */
		flags = O_RDWR | (create_trunc ? O_CREAT | O_TRUNC : 0);

	int fd = ::open(path.c_str(), flags, 0666);
	if (fd < 0) return {};
	f->set_fd(fd);
#endif

	return f;
}

} // namespace mpp
