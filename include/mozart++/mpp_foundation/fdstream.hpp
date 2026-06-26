/**
 * Mozart++ Template Library
 * Licensed under Apache 2.0
 * Copyright (C) 2020-2021 Chengdu Covariant Technologies Co., LTD.
 * Website: https://covariant.cn/
 * Github:  https://github.com/chengdu-zhirui/
 */
#pragma once

#include "io.hpp"

#include <algorithm>
#include <istream>
#include <limits>
#include <ostream>
#include <streambuf>

namespace mpp {
	class fdoutbuf : public std::streambuf {
	private:
		mpp::fd_type _fd;
		bool _valid = true;

	public:
		explicit fdoutbuf(mpp::fd_type fd)
			: _fd(fd)
		{
		}

		/**
		 * Mark the underlying fd as no longer valid so that subsequent
		 * writes are silently discarded instead of writing to a closed
		 * or stale handle (which could be unsafe on Windows if the
		 * handle value has been reused by the OS).
		 */
		void invalidate()
		{
			_valid = false;
		}

	protected:
		int_type overflow(int_type c) override
		{
			if (!_valid)
				return EOF;
			if (c != EOF) {
				char z = c;
				if (mpp::write(_fd, &z, 1) != 1) {
					return EOF;
				}
			}
			return c;
		}

		std::streamsize xsputn(const char *s,
		                       std::streamsize num) override
		{
			if (!_valid)
				return 0;
			if (num <= 0) return 0;
			std::streamsize total = 0;
			while (total < num) {
				size_t chunk = static_cast<size_t>(num - total);
#ifdef MOZART_PLATFORM_WIN32
				// WriteFile accepts DWORD (32-bit); limit chunks to avoid
				// silent truncation on very large writes (> 4 GiB).
				constexpr size_t max_chunk =
				    static_cast<size_t>(std::numeric_limits<DWORD>::max());
				if (chunk > max_chunk) chunk = max_chunk;
#endif
				mpp::ssize_t written = mpp::write(_fd, s + total, chunk);
				if (written <= 0) break;
				total += written;
			}
			return total;
		}
	};

	class fdostream : public std::ostream {
	private:
		// _buf must be declared before it is used in rdbuf(); std::ostream(nullptr)
		// is implementation-defined but supported by all major implementations
		// (libstdc++, libc++, MSVC). The subsequent rdbuf(&_buf) call establishes
		// a valid streambuf before any I/O occurs.
		fdoutbuf _buf;
	public:
		explicit fdostream(fd_type fd)
			: std::ostream(nullptr), _buf(fd)
		{
			rdbuf(&_buf);
		}

		/**
		 * Mark the underlying fd as no longer valid.  After this call,
		 * writes to the stream are silently discarded.
		 */
		void invalidate()
		{
			_buf.invalidate();
		}


#ifdef MOZART_PLATFORM_WIN32

		explicit fdostream(int cfd)
			: fdostream(reinterpret_cast<fd_type>(_get_osfhandle(cfd))) {}

#endif
	};

	class fdinbuf : public std::streambuf {
	private:
		mpp::fd_type _fd;

	protected:
		/**
		 * size of putback area
		 */
		static constexpr size_t PUTBACK_SIZE = 4;

		/**
		 * size of the data buffer
		 */
		static constexpr size_t BUFFER_SIZE = 1024;

		char _buffer[BUFFER_SIZE + PUTBACK_SIZE] {0};

	public:
		explicit fdinbuf(mpp::fd_type fd)
			: _fd(fd)
		{
			setg(_buffer + PUTBACK_SIZE,     // beginning of putback area
			     _buffer + PUTBACK_SIZE,     // read position
			     _buffer + PUTBACK_SIZE);    // end position
		}

	protected:
		// insert new characters into the buffer
		int_type underflow() override
		{
			// is read position before end of buffer?
			if (gptr() < egptr()) {
				return traits_type::to_int_type(*gptr());
			}

			// handle putback area
			size_t backSize = gptr() - eback();
			if (backSize > PUTBACK_SIZE) {
				backSize = PUTBACK_SIZE;
			}

			// copy up to PUTBACK_SIZE characters previously read into
			// the putback area
			std::memmove(_buffer + (PUTBACK_SIZE - backSize),
			             gptr() - backSize,
			             backSize);

			// read at most BUFFER_SIZE new characters
			int num = mpp::read(_fd, _buffer + PUTBACK_SIZE, BUFFER_SIZE);
			if (num <= 0) {
				// it might be error happened somewhere or EOF encountered
				// we simply return EOF
				return EOF;
			}

			// reset buffer pointers
			setg(_buffer + (PUTBACK_SIZE - backSize),
			     _buffer + PUTBACK_SIZE,
			     _buffer + PUTBACK_SIZE + num);

			// return next character
			return traits_type::to_int_type(*gptr());
		}
	};

	class fdistream : public std::istream {
	private:
		fdinbuf _buf;
	public:
		explicit fdistream(fd_type fd)
			: std::istream(nullptr), _buf(fd)
		{
			rdbuf(&_buf);
		}

#ifdef MOZART_PLATFORM_WIN32

		explicit fdistream(int cfd)
			: fdistream(reinterpret_cast<fd_type>(_get_osfhandle(cfd))) {}

#endif
	};
}

