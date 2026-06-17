/**
 * Mozart++ Template Library
 * Licensed under Apache 2.0
 * Copyright (C) 2020-2021 Chengdu Covariant Technologies Co., LTD.
 * Website: https://covariant.cn/
 * Github:  https://github.com/chengdu-zhirui/
 */
#include <mozart++/core>

#ifdef MOZART_PLATFORM_WIN32

#include <mozart++/process>

#include <Windows.h>

namespace mpp_impl {
	int wait_for(const process_info &info)
	{
		WaitForSingleObject(info._pid, INFINITE);
		DWORD code = 0;
		GetExitCodeProcess(info._pid, &code);
		return code;
	}

	void terminate_process(const process_info &info, bool force)
	{
		TerminateProcess(info._pid, 0);
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
		// Delegate directly to the OS — no polling loop needed on Windows.
		DWORD result = WaitForSingleObject(info._pid, static_cast<DWORD>(timeout_ms));
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
