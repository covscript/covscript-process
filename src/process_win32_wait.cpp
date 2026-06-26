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

#ifdef MOZART_PLATFORM_WIN32

#include <mozart++/process>

#include <Windows.h>
#include <TlHelp32.h>

#include <unordered_map>
#include <vector>

namespace {
	void terminate_by_pid(DWORD pid, UINT exit_code)
	{
		if (pid == 0 || pid == GetCurrentProcessId())
			return;

		HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
		if (h == nullptr)
			return;

		TerminateProcess(h, exit_code);
		CloseHandle(h);
	}

	void terminate_descendants(DWORD root_pid, UINT exit_code)
	{
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap == INVALID_HANDLE_VALUE)
			return;

		std::unordered_map<DWORD, std::vector<DWORD>> children;
		PROCESSENTRY32 pe;
		pe.dwSize = sizeof(pe);
		if (Process32First(snap, &pe)) {
			do {
				children[pe.th32ParentProcessID].push_back(pe.th32ProcessID);
			} while (Process32Next(snap, &pe));
		}
		CloseHandle(snap);

		std::vector<DWORD> stack;
		std::vector<DWORD> order;
		auto it = children.find(root_pid);
		if (it != children.end())
			stack = it->second;

		while (!stack.empty()) {
			DWORD pid = stack.back();
			stack.pop_back();
			order.push_back(pid);

			auto cit = children.find(pid);
			if (cit != children.end()) {
				for (auto child : cit->second)
					stack.push_back(child);
			}
		}

		for (auto rit = order.rbegin(); rit != order.rend(); ++rit)
			terminate_by_pid(*rit, exit_code);
	}
}

namespace mpp_impl {
	int wait_for(const process_info &info)
	{
		WaitForSingleObject(info._pid, INFINITE);
		DWORD code = 0;
		GetExitCodeProcess(info._pid, &code);
		return code;
	}
	uint64_t get_process_start_time(int /*pid*/)
	{
		// Windows path currently records identity at process creation time
		// (process_info::_start_time) and does not query it here by PID.
		// Return 0 to signal "not recorded" for this helper.
		return 0;
	}

	void terminate_process(const process_info &info, bool force)
	{
		// CNI_API.md §6: kill(force=false) → SIGTERM (143), kill(force=true) → SIGKILL (137).
		TerminateProcess(info._pid, force ? 137 : 143);
	}

	void terminate_process_tree(const process_info &info, bool force)
	{
		// If the root process is already gone, avoid traversing descendants by PID
		// because the PID may have been recycled by an unrelated process tree.
		if (process_exited(info))
			return;

		// The exit code convention is now consistently applied to both root
		// and descendants.
		const UINT code = force ? 137 : 143;
		DWORD root_pid = GetProcessId(info._pid);
		if (root_pid != 0)
			terminate_descendants(root_pid, code);
		TerminateProcess(info._pid, code);
	}

	bool process_exited(const process_info &info)
	{
		// Use WaitForSingleObject with zero timeout for a reliable non-blocking check.
		// GetExitCodeProcess + STILL_ACTIVE comparison is unreliable: a process that
		// legitimately exits with code 259 (STILL_ACTIVE) would appear to still be running.
		return WaitForSingleObject(info._pid, 0) == WAIT_OBJECT_0;
	}

	bool wait_timeout_ms(const process_info &info, int timeout_ms, int &exit_code,
	                     int /*poll_interval_ms*/)
	{
		// Normalize: negative timeout → INFINITE (same semantics as Unix after fix).
		// Explicit cast avoids relying on implicit DWORD conversion of -1.
		DWORD dwTimeout = (timeout_ms < 0) ? INFINITE : static_cast<DWORD>(timeout_ms);
		DWORD result = WaitForSingleObject(info._pid, dwTimeout);
		if (result == WAIT_OBJECT_0) {
			DWORD code = 0;
			GetExitCodeProcess(info._pid, &code);
			exit_code = static_cast<int>(code);
			return true;
		}
		return false; // WAIT_TIMEOUT or error
	}

	int get_pid(const process_info &info)
	{
		return static_cast<int>(GetProcessId(info._pid));
	}
}

#endif
