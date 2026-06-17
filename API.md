# CovScript Process API 最终版

## 1. 决策摘要

- 异步后端统一采用 libuv。
- 旧接口与兼容层全部删除，不保留 legacy 后端。
- redirect_out/redirect_err 只接受文件对象，不接受路径字符串。
- 不新增 process 专用“按块读取”接口，统一复用异步文件 I/O 能力。

解释器基线：

- 当前解释器 ABI：`COVSCRIPT_ABI_VERSION = 260602`（v3.4.8）。
- 扩展内部 fiber 协作门槛 ABI：`>= 250908`，在当前解释器上默认走 fiber 兼容路径。

## 2. 对外 API

### 2.1 事件循环（基础外露）

- process.async.poll() -> integer
- process.async.poll_once() -> bool
- process.async.restart()
- process.async.stop()

说明：

- poll 运行直到当前队列空闲，返回本轮处理的事件数量。
- poll_once 仅推进一次事件分发，返回是否处理了事件。
- restart 用于 stop 后重新进入可运行状态。
- 仅暴露基础控制面；底层后端细节仍保持内部封装。
- 同一个 loop 只能由一个线程驱动；并发调用 poll/poll_once 属于未定义行为。

### 2.2 异步文件 I/O

- process.async.fstream(path: str, mode: str) -> file_t | null
- file_t.out() -> std::istream &
- file_t.in() -> std::ostream &
- file_t.read(size: int, deadline_ms: int = -1) -> str | null
- file_t.write(data: str, deadline_ms: int = -1) -> int
- file_t.flush(deadline_ms: int = -1) -> bool
- file_t.close()

说明：

- file_t 是重定向唯一绑定对象。
- 文件对象持有底层句柄生命周期，process 不再额外托管该状态。
- 对外以标准流引用暴露（std::istream / std::ostream），并由 libuv `uv_fs_*` 路径执行读写/刷盘。
- read 返回 null 表示超时、取消、错误或文件已关闭；空字符串表示读到 EOF。
- write 返回实际写入字节数；flush 返回是否在 deadline 内完成。

### 2.3 进程启动

- process.exec(executable: str, args: array) -> process_t
- process.shell(command: str) -> process_t
- process.builder.start() -> process_t

### 2.4 process.builder

- cmd(value: str)
- arg(values: array)
- dir(value: str)
- env(key: str, value: str)
- inherit_env(value: bool)
- inherit_output(value: bool)
- merge_output(value: bool)
- use_shell(program: str)
- redirect_in(file: file_t)
- redirect_out(file: file_t)
- redirect_err(file: file_t)
- start() -> process_t

### 2.5 process_t

- in / out / err
- wait() -> int
- try_wait() -> int | null
- wait_poll(timeout_ms: int, poll_interval_ms: int = 5) -> int | null
- wait_with(timeout_ms: int, callback: callable) -> int | null
- wait_for(ms: int) -> bool
- wait_until(deadline_ms: int) -> bool
- poll() -> bool
- has_exited() -> bool
- is_running() -> bool
- get_pid() -> int
- kill(force: bool = true)
- kill_tree(force: bool = true)
- communicate() -> [out: str/null, err: str/null, exit_code: int]

## 3. 统一语义约束

- 不暴露 send_signal。
- 脚本侧根级快捷 shell 启动入口为 `process.shell(command)`。
- builder 侧 shell 入口为 `use_shell(program)`；`shell_off()` 仅保留在 mpp C++ 层。
- 脚本侧请使用 `import process`，避免与解释器内置 `process` 冲突。
- merge_output=true 时，redirect_err 无效。
- inherit_output=true 时不采集 out/err，communicate 的 out/err 为 null。
- 参数优先级：merge_output > redirect_out/redirect_err > inherit_output。
- redirect_in/redirect_out/redirect_err 仅接受 file_t；当前 CNI 不提供 clear_* 入口。
- 参数类型错误抛异常；运行时失败使用 null/false 返回值表达。
- wait_for/wait_until 超时返回 false，不抛异常。
- wait_with 的 callback 若抛异常，应立即中断 wait_with 并向上抛出；子进程保持原生运行状态，不做隐式 kill。
- `process.async` 当前仅保留一个命名空间，不再提供 `process.aio` 别名。
- `file_t` 的 deadline 语义由 libuv 请求取消驱动；超时后返回失败值，不保证底层 OS I/O 立刻终止。
- `restart()` 为语义对称接口；当前实现是 no-op（libuv 在 stop 后可直接继续 uv_run）。
- **mpp vs CNI 规约**：mpp C++ 层与脚本 CNI 层不要求 1:1 对齐。mpp 保留 C++ 易用性接口（如 `shell(nullptr_t)`、`wait_timeout_ms()`、`begin_wait/poll_wait/collect_wait`、链式调用），CNI 仅暴露脚本侧需要且稳定的子集。
- communicate 在进程异常终止时仍返回三元组，exit_code 采用统一编码：
	- 正常退出：返回原始进程退出码（0..255）。
	- 信号终止：返回 128 + signum（如 SIGINT=130、SIGTERM=143、SIGKILL=137）。
	- Windows 映射：
		- kill(force=false) 视为 SIGTERM，返回 143。
		- kill(force=true) 视为 SIGKILL，返回 137。
		- 其他外部终止若无法识别信号，统一按 SIGTERM 语义返回 143。

## 4. 实施计划

### Phase 1: API 收口

- 删除 CNI 的 shell_cmd、pid、send_signal 入口。
- builder 侧只保留 `use_shell(program)` 作为脚本入口。
- 增加 process.shell(command) 与 get_pid。

### Phase 2: mpp 收口

- process_builder 删除 shell(bool)/shell_command(str) 公开语义。
- start() 仅保留 shell=null 与 shell=str 两条执行路径。

### Phase 3: libuv 与异步文件 I/O

- 基于 libuv 接入统一事件循环与调度机制（外露基础 loop API：poll/poll_once/restart/stop）。
- mpp 引入 file_t 异步文件抽象。
- redirect_in/redirect_out/redirect_err 完成到 file_t 的绑定迁移。

### Phase 4: 增强能力

- wait/communicate 全量接入 deadline 语义。
- stdin 异步写入队列与背压。
- 基础可观测事件（spawn/read/write/wait/kill/timeout）。

## 5. 验收标准

- 现有 test_unit.csc 全量通过。
- 现有 test_async.csc 全量通过。
- 现有 test_file_redirect.csc 全量通过。
- 大输出 communicate 无死锁、无截断。
- wait_poll/wait_with 高频场景 CPU 不高于当前版本。
- Windows 与 Unix 构建通过。
- file_t 重定向场景下，process 销毁后文件对象仍可按其自身语义安全关闭。
