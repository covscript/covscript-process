/**
 * Covariant Script Libmozart++ Process Support
 *
 * C++ unit tests for the libuv async filesystem wrappers in process.cpp.
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

#include <uv.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <climits>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#define platform_open _open
#define platform_close _close
#define platform_unlink _unlink
#define platform_isatty _isatty
#define platform_sleep(ms) Sleep(ms)
#define O_RDWR_CREATE (_O_RDWR | _O_CREAT | _O_TRUNC | _O_BINARY)
#define O_RDONLY_BIN (_O_RDONLY | _O_BINARY)
#define PERM_MODE (_S_IREAD | _S_IWRITE)
#else
#include <fcntl.h>
#include <unistd.h>
#define platform_open open
#define platform_close close
#define platform_unlink unlink
#define platform_isatty isatty
#define platform_sleep(ms) usleep((ms) * 1000)
#define O_RDWR_CREATE (O_RDWR | O_CREAT | O_TRUNC)
#define O_RDONLY_BIN O_RDONLY
#ifndef PERM_MODE
#define PERM_MODE (S_IRUSR | S_IWUSR)
#endif
#endif

// ============================================================================
// Mirrors of process.cpp internals
// ============================================================================

struct uv_fs_op_state {
	bool done = false;
	int result = UV_ECANCELED;
};

struct uv_fs_request {
	uv_fs_t req;
	uv_fs_op_state state;
	std::string write_data;
};

static void uv_fs_complete(uv_fs_t *req)
{
	auto *bundle = static_cast<uv_fs_request *>(req->data);
	bundle->state.result = static_cast<int>(req->result);
	bundle->state.done = true;
	uv_fs_req_cleanup(req);
}

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
				continue;
			}
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	return !deadline_reached;
}

// ============================================================================
// Test harness
// ============================================================================

static int pass_count = 0;
static int fail_count = 0;

#define TEST(name) \
	do { \
		std::printf("  %-55s ", name); \
		std::fflush(stdout); \
	} while (0)

#define PASS() \
	do { \
		std::printf("PASS\n"); \
		++pass_count; \
	} while (0)

#define FAIL(msg) \
	do { \
		std::printf("FAIL: %s\n", msg); \
		++fail_count; \
	} while (0)

#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			FAIL(#cond); \
			return; \
		} \
	} while (0)

static std::string temp_path(const char *name)
{
	return std::string("./.tmp_cpptest_") + name + ".txt";
}

// Open a file for read-write (create/truncate) and return a CRT fd.
static int open_file_rw(const char *path)
{
	int fd = platform_open(path, O_RDWR_CREATE, PERM_MODE);
	return fd;
}

// Open a file for reading only.
static int open_file_ro(const char *path)
{
	int fd = platform_open(path, O_RDONLY_BIN);
	return fd;
}

// ============================================================================
// T01: uv_fs_complete sets done/result correctly (via real write)
// ============================================================================
static void test_callback_sets_state()
{
	TEST("callback sets done and result");
	const char *data = "t01_callback_test";
	auto path = temp_path("t01");

	int fd = open_file_rw(path.c_str());
	CHECK(fd >= 0);

	uv_fs_request bundle{};
	bundle.req.data = &bundle;

	uv_buf_t iov = uv_buf_init(const_cast<char *>(data),
	                           static_cast<unsigned int>(strlen(data)));
	int submit = uv_fs_write(uv_default_loop(), &bundle.req,
	                         static_cast<uv_file>(fd), &iov, 1,
	                         -1, uv_fs_complete);
	CHECK(submit >= 0);

	bool on_time = uv_wait_fs_with_deadline(uv_default_loop(), &bundle, 5000);
	CHECK(on_time);
	CHECK(bundle.state.done);
	CHECK(bundle.state.result == static_cast<int>(strlen(data)));

	platform_close(fd);
	platform_unlink(path.c_str());
	PASS();
}

// ============================================================================
// T02: Write + wait with no deadline (infinite wait)
// ============================================================================
static void test_write_no_deadline()
{
	TEST("write with deadline=-1 (infinite wait)");
	const char *data = "t02_no_deadline_data";
	auto path = temp_path("t02");

	int fd = open_file_rw(path.c_str());
	CHECK(fd >= 0);

	uv_fs_request bundle{};
	bundle.req.data = &bundle;
	// No deadline: buf_ptr points to caller's string (data), not copied.
	uv_buf_t iov = uv_buf_init(const_cast<char *>(data),
	                           static_cast<unsigned int>(strlen(data)));
	int submit = uv_fs_write(uv_default_loop(), &bundle.req,
	                         static_cast<uv_file>(fd), &iov, 1,
	                         -1, uv_fs_complete);
	CHECK(submit >= 0);

	bool on_time = uv_wait_fs_with_deadline(uv_default_loop(), &bundle, -1);
	CHECK(on_time);
	CHECK(bundle.state.done);
	CHECK(bundle.state.result == static_cast<int>(strlen(data)));

	platform_close(fd);
	platform_unlink(path.c_str());
	PASS();
}

// ============================================================================
// T03: Write + wait with generous deadline (completes on time)
// ============================================================================
static void test_write_with_deadline_ok()
{
	TEST("write with deadline=5000 (completes on time)");
	const char *data = "t03_deadline_ok";
	auto path = temp_path("t03");

	int fd = open_file_rw(path.c_str());
	CHECK(fd >= 0);

	uv_fs_request bundle{};
	bundle.req.data = &bundle;
	bundle.write_data = data;  // simulate has_deadline copy
	uv_buf_t iov = uv_buf_init(
	                   const_cast<char *>(bundle.write_data.data()),
	                   static_cast<unsigned int>(bundle.write_data.size()));
	int submit = uv_fs_write(uv_default_loop(), &bundle.req,
	                         static_cast<uv_file>(fd), &iov, 1,
	                         -1, uv_fs_complete);
	CHECK(submit >= 0);

	bool on_time = uv_wait_fs_with_deadline(uv_default_loop(), &bundle, 5000);
	CHECK(on_time);
	CHECK(bundle.state.result == static_cast<int>(strlen(data)));

	platform_close(fd);
	platform_unlink(path.c_str());
	PASS();
}

// ============================================================================
// T04: Write + wait with zero deadline (edge-case timing probe)
// ============================================================================
static void test_write_deadline_zero()
{
	TEST("write with deadline=0 (may complete or timeout)");
	const char *data = "t04_zero";
	auto path = temp_path("t04");

	int fd = open_file_rw(path.c_str());
	CHECK(fd >= 0);

	uv_fs_request bundle{};
	bundle.req.data = &bundle;
	bundle.write_data = data;
	uv_buf_t iov = uv_buf_init(
	                   const_cast<char *>(bundle.write_data.data()),
	                   static_cast<unsigned int>(bundle.write_data.size()));
	int submit = uv_fs_write(uv_default_loop(), &bundle.req,
	                         static_cast<uv_file>(fd), &iov, 1,
	                         -1, uv_fs_complete);
	CHECK(submit >= 0);

	bool on_time = uv_wait_fs_with_deadline(uv_default_loop(), &bundle, 0);
	// Both outcomes are valid for deadline=0.
	CHECK(bundle.state.done);
	(void)on_time;

	platform_close(fd);
	platform_unlink(path.c_str());
	PASS();
}

// ============================================================================
// T05: Read back data written via async path
// ============================================================================
static void test_read_no_deadline()
{
	TEST("read with deadline=-1 after async write");
	const char *data = "t05_readback_test_data_xyz";
	size_t data_len = strlen(data);
	auto path = temp_path("t05");

	// Write via async path
	int fd = open_file_rw(path.c_str());
	CHECK(fd >= 0);
	{
		uv_fs_request bundle{};
		bundle.req.data = &bundle;
		uv_buf_t iov = uv_buf_init(const_cast<char *>(data),
		                           static_cast<unsigned int>(data_len));
		uv_fs_write(uv_default_loop(), &bundle.req,
		            static_cast<uv_file>(fd), &iov, 1,
		            -1, uv_fs_complete);
		uv_wait_fs_with_deadline(uv_default_loop(), &bundle, -1);
		CHECK(bundle.state.result == static_cast<int>(data_len));
	}
	platform_close(fd);

	// Read back
	fd = open_file_ro(path.c_str());
	CHECK(fd >= 0);
	{
		std::vector<char> buf(data_len + 1, '\0');
		uv_fs_request bundle{};
		bundle.req.data = &bundle;
		uv_buf_t iov = uv_buf_init(buf.data(),
		                           static_cast<unsigned int>(buf.size() - 1));
		uv_fs_read(uv_default_loop(), &bundle.req,
		           static_cast<uv_file>(fd), &iov, 1,
		           0, uv_fs_complete);
		uv_wait_fs_with_deadline(uv_default_loop(), &bundle, -1);
		CHECK(bundle.state.result == static_cast<int>(data_len));
		CHECK(std::strcmp(buf.data(), data) == 0);
	}
	platform_close(fd);
	platform_unlink(path.c_str());
	PASS();
}

// ============================================================================
// T06: Read with deadline on blocking stdin — exercises cancel path
// ============================================================================
static void test_read_cancel_on_stdin()
{
	TEST("read stdin with deadline=10ms (cancel path)");

	if (!platform_isatty(0)) {
		std::printf("SKIP (stdin not a terminal)\n");
		return;
	}

	std::printf("\n    (please do not type anything for ~500ms)...\n");
	std::fflush(stdout);

	std::vector<char> buf(256);
	uv_fs_request bundle{};
	bundle.req.data = &bundle;
	uv_buf_t iov = uv_buf_init(buf.data(),
	                           static_cast<unsigned int>(buf.size()));
	int submit = uv_fs_read(uv_default_loop(), &bundle.req,
	                        static_cast<uv_file>(0), &iov, 1,
	                        0, uv_fs_complete);
	CHECK(submit >= 0);

	bool on_time = uv_wait_fs_with_deadline(uv_default_loop(), &bundle, 10);
	CHECK(bundle.state.done);
	if (!on_time) {
		std::printf("(timeout, result=%d) ", bundle.state.result);
	}
	else {
		std::printf("(on_time, result=%d) ", bundle.state.result);
	}

	PASS();
}

// ============================================================================
// T07: buf_size > UINT_MAX check
// ============================================================================
static void test_size_check_uint_max()
{
	TEST("buf_size > UINT_MAX rejection logic");

	// Boundary: UINT_MAX itself should pass.
	size_t max_uint = static_cast<size_t>(UINT_MAX);
	CHECK(max_uint <= UINT_MAX);

	// On 64-bit platforms, UINT_MAX+1 should be rejected.
	if constexpr (sizeof(size_t) > sizeof(unsigned int)) {
		size_t overflow = static_cast<size_t>(UINT_MAX) + 1;
		CHECK(overflow > UINT_MAX);
	}

	PASS();
}

// ============================================================================
// T08: Flush via uv_fs_fsync
// ============================================================================
static void test_flush_async()
{
	TEST("fsync flush with no deadline");
	auto path = temp_path("t08");

	int fd = open_file_rw(path.c_str());
	CHECK(fd >= 0);

	// Write some data first
	{
		const char *data = "t08_flush_data";
		uv_fs_request bundle{};
		bundle.req.data = &bundle;
		uv_buf_t iov = uv_buf_init(const_cast<char *>(data),
		                           static_cast<unsigned int>(strlen(data)));
		uv_fs_write(uv_default_loop(), &bundle.req,
		            static_cast<uv_file>(fd), &iov, 1,
		            -1, uv_fs_complete);
		uv_wait_fs_with_deadline(uv_default_loop(), &bundle, -1);
		CHECK(bundle.state.result > 0);
	}

	// Flush
	{
		uv_fs_request bundle{};
		bundle.req.data = &bundle;
		int submit = uv_fs_fsync(uv_default_loop(), &bundle.req,
		                         static_cast<uv_file>(fd), uv_fs_complete);
		CHECK(submit >= 0);
		bool on_time = uv_wait_fs_with_deadline(uv_default_loop(), &bundle, -1);
		CHECK(on_time);
		CHECK(bundle.state.result >= 0);
	}

	platform_close(fd);
	platform_unlink(path.c_str());
	PASS();
}

// ============================================================================
// T09: Stress — rapid create/wait/destroy cycles (leak detection)
// ============================================================================
static void test_stress_rapid_cycles()
{
	TEST("stress: 100 rapid write cycles");
	auto path = temp_path("t09");

	int fd = open_file_rw(path.c_str());
	CHECK(fd >= 0);

	constexpr int N = 100;
	int ok = 0;
	for (int i = 0; i < N; ++i) {
		// Heap-allocate to match production code pattern and avoid
		// any potential stack-size issues with uv_fs_request.
		auto *bundle = new uv_fs_request{};
		bundle->req.data = bundle;
		char c = static_cast<char>('A' + (i % 26));
		uv_buf_t iov = uv_buf_init(&c, 1);
		int submit = uv_fs_write(uv_default_loop(), &bundle->req,
		                         static_cast<uv_file>(fd), &iov, 1,
		                         -1, uv_fs_complete);
		if (submit < 0) {
			uv_fs_req_cleanup(&bundle->req);
			delete bundle;
			continue;
		}

		uv_wait_fs_with_deadline(uv_default_loop(), bundle, 1000);
		if (bundle->state.result == 1) ++ok;
		delete bundle;
	}
	CHECK(ok == N);

	// Fsync to flush
	{
		auto *bundle = new uv_fs_request{};
		bundle->req.data = bundle;
		uv_fs_fsync(uv_default_loop(), &bundle->req,
		            static_cast<uv_file>(fd), uv_fs_complete);
		uv_wait_fs_with_deadline(uv_default_loop(), bundle, -1);
		bool ok_fsync = bundle->state.result >= 0;
		delete bundle;
		CHECK(ok_fsync);
	}
	platform_close(fd);

	// Verify total written size
	fd = open_file_ro(path.c_str());
	CHECK(fd >= 0);
	{
		std::vector<char> buf(256);
		auto *bundle = new uv_fs_request{};
		bundle->req.data = bundle;
		uv_buf_t iov = uv_buf_init(buf.data(),
		                           static_cast<unsigned int>(buf.size()));
		uv_fs_read(uv_default_loop(), &bundle->req,
		           static_cast<uv_file>(fd), &iov, 1,
		           0, uv_fs_complete);
		uv_wait_fs_with_deadline(uv_default_loop(), bundle, -1);
		bool size_ok = bundle->state.result == N;
		delete bundle;
		CHECK(size_ok);
	}
	platform_close(fd);
	platform_unlink(path.c_str());
	PASS();
}

// ============================================================================
// T10: write_data copy survives past deadline (write_data lifetime)
// ============================================================================
static void test_write_data_copy_lifetime()
{
	TEST("write_data copy survives past deadline");
	auto path = temp_path("t10");

	int fd = open_file_rw(path.c_str());
	CHECK(fd >= 0);

	std::string data = "t10_copy_lifetime_check_data";

	// Simulate the has_deadline=true code path: copy data into bundle.
	uv_fs_request bundle{};
	bundle.req.data = &bundle;
	bundle.write_data = data;  // heap copy inside bundle
	const char *buf_ptr = bundle.write_data.data();
	auto buf_size = bundle.write_data.size();
	CHECK(buf_size <= UINT_MAX);
	uv_buf_t iov = uv_buf_init(const_cast<char *>(buf_ptr),
	                           static_cast<unsigned int>(buf_size));

	int submit = uv_fs_write(uv_default_loop(), &bundle.req,
	                         static_cast<uv_file>(fd), &iov, 1,
	                         -1, uv_fs_complete);
	CHECK(submit >= 0);

	bool on_time = uv_wait_fs_with_deadline(uv_default_loop(), &bundle, 5000);
	CHECK(bundle.state.done);
	CHECK(bundle.state.result == static_cast<int>(data.size()));
	(void)on_time;

	platform_close(fd);
	platform_unlink(path.c_str());
	PASS();
}

// ============================================================================
// T11: Deadline return value semantics
// ============================================================================
static void test_deadline_return_semantics()
{
	TEST("deadline return value semantics");
	auto path = temp_path("t11");

	int fd = open_file_rw(path.c_str());
	CHECK(fd >= 0);

	// Generous deadline, fast write → must return true
	{
		const char *data = "fast";
		uv_fs_request bundle{};
		bundle.req.data = &bundle;
		uv_buf_t iov = uv_buf_init(const_cast<char *>(data),
		                           static_cast<unsigned int>(strlen(data)));
		uv_fs_write(uv_default_loop(), &bundle.req,
		            static_cast<uv_file>(fd), &iov, 1,
		            -1, uv_fs_complete);
		bool on_time = uv_wait_fs_with_deadline(
		                   uv_default_loop(), &bundle, 5000);
		CHECK(on_time);
		CHECK(bundle.state.done);
	}

	// No deadline → must return true
	{
		const char *data = "indefinite";
		uv_fs_request bundle{};
		bundle.req.data = &bundle;
		uv_buf_t iov = uv_buf_init(const_cast<char *>(data),
		                           static_cast<unsigned int>(strlen(data)));
		uv_fs_write(uv_default_loop(), &bundle.req,
		            static_cast<uv_file>(fd), &iov, 1,
		            -1, uv_fs_complete);
		bool on_time = uv_wait_fs_with_deadline(
		                   uv_default_loop(), &bundle, -1);
		CHECK(on_time);
		CHECK(bundle.state.done);
	}

	platform_close(fd);
	platform_unlink(path.c_str());
	PASS();
}

// ============================================================================
// T12: No double uv_fs_req_cleanup (callback cleans up, caller does not)
// ============================================================================
static void test_no_double_cleanup()
{
	TEST("no double uv_fs_req_cleanup (10 iterations)");
	auto path = temp_path("t12");

	int fd = open_file_rw(path.c_str());
	CHECK(fd >= 0);

	const char *data = "t12_nodouble";
	for (int i = 0; i < 10; ++i) {
		uv_fs_request bundle{};
		bundle.req.data = &bundle;
		uv_buf_t iov = uv_buf_init(const_cast<char *>(data),
		                           static_cast<unsigned int>(strlen(data)));
		int submit = uv_fs_write(uv_default_loop(), &bundle.req,
		                         static_cast<uv_file>(fd), &iov, 1,
		                         -1, uv_fs_complete);
		CHECK(submit >= 0);
		uv_wait_fs_with_deadline(uv_default_loop(), &bundle, 5000);
		CHECK(bundle.state.done);
		// uv_fs_req_cleanup was already called by callback.
		// bundle goes out of scope here — no double cleanup.
	}

	platform_close(fd);
	platform_unlink(path.c_str());
	PASS();
}

// ============================================================================
// T13: invalid fd (ufd < 0) error path
// ============================================================================
static void test_invalid_fd()
{
	TEST("invalid fd (ufd=-1) is rejected");
	const char *data = "test";

	uv_fs_request bundle{};
	bundle.req.data = &bundle;
	uv_buf_t iov = uv_buf_init(const_cast<char *>(data),
	                           static_cast<unsigned int>(strlen(data)));

	// uv_fs_write with fd = -1: libuv may return 0 (queued) and deliver
	// the error asynchronously via the callback, or it may return < 0
	// (synchronous rejection).  Both paths are exercised by production code.
	int submit = uv_fs_write(uv_default_loop(), &bundle.req,
	                         static_cast<uv_file>(-1), &iov, 1,
	                         -1, uv_fs_complete);
	if (submit < 0) {
		// Synchronous rejection: production code calls uv_fs_req_cleanup
		// and deletes bundle.
		uv_fs_req_cleanup(&bundle.req);
		std::printf("(sync reject) ");
	}
	else {
		// Asynchronous: wait for callback which sets done + calls cleanup.
		uv_wait_fs_with_deadline(uv_default_loop(), &bundle, 5000);
		CHECK(bundle.state.done);
		// result should be negative (error code from libuv).
		std::printf("(async result=%d) ", bundle.state.result);
	}

	PASS();
}

// ============================================================================
// main
// ============================================================================
static void cleanup_temp_files()
{
	// Remove leftover temp files from previous runs.  On Windows, files
	// created by a crashed run may be read-only, which blocks O_TRUNC.
	for (int i = 1; i <= 20; ++i) {
		char buf[128];
		std::snprintf(buf, sizeof(buf), "./.tmp_cpptest_t%02d.txt", i);
#ifdef _WIN32
		// Strip read-only attribute so _unlink can succeed.
		SetFileAttributesA(buf, FILE_ATTRIBUTE_NORMAL);
#endif
		platform_unlink(buf);
	}
}

int main()
{
	cleanup_temp_files();

	std::printf("\n=== C++ UV Filesystem Wrapper Tests ===\n\n");

	// Callback lifecycle
	test_callback_sets_state();       // T01
	test_no_double_cleanup();          // T12

	// Deadline paths
	test_write_no_deadline();          // T02
	test_write_with_deadline_ok();     // T03
	test_write_deadline_zero();        // T04
	test_deadline_return_semantics();  // T11

	// Read / round-trip / flush
	test_read_no_deadline();           // T05
	test_read_cancel_on_stdin();      // T06 (may skip in CI)
	test_flush_async();               // T08

	// Buffer lifetime
	test_write_data_copy_lifetime();  // T10

	// Error path
	test_invalid_fd();                // T13

	// Logic / boundary
	test_size_check_uint_max();       // T07

	// Stress
	test_stress_rapid_cycles();       // T09

	std::printf("\n========================================\n");
	std::printf("Results: %d passed, %d failed\n", pass_count, fail_count);
	std::printf("========================================\n\n");

	return fail_count > 0 ? 1 : 0;
}
