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

- process.async.fstream(path: str, mode: str, [options]) -> file_t | null
- file_t.out() -> std::istream &
- file_t.in() -> std::ostream &
- file_t.read(file: file_t, size: int, [deadline_ms]) -> str | null
- file_t.write(file: file_t, data: str, [deadline_ms]) -> int
- file_t.flush(file: file_t, [deadline_ms]) -> bool
- file_t.close()

说明：

- file_t 是重定向唯一绑定对象。
- 文件对象持有底层句柄生命周期，process 不再额外托管该状态。
- 对外以标准流引用暴露（std::istream / std::ostream），异步调度由内部后端处理。
- read 返回 null 表示超时或文件已关闭；空字符串表示读到 EOF（该语义固定，不再拆分额外状态码）。
- write 返回实际写入字节数；flush 返回是否在 deadline 内完成。

### 2.3 进程启动

- process.exec(executable: str, args: array, [options]) -> process_t | null
- process.builder.start() + shell(...) 作为 shell 启动入口

options 字段：

- dir: str = "."
- env: map = {}
- inherit_env: bool = true
- inherit_output: bool = false (shell 默认 true)
- merge_output: bool = false (shell 默认 true)
- shell: null | str

### 2.4 process.builder

- cmd(value: str)
- arg(values: array)
- dir(value: str)
- env(key: str, value: str)
- inherit_env(value: bool)
- inherit_output(value: bool)
- merge_output(value: bool)
- use_shell(program: str)
- shell_off()
- redirect_out(file: file_t)
- clear_redirect_out()
- redirect_err(file: file_t)
- clear_redirect_err()
- start() -> process_t

### 2.5 process_t

- in / out / err
- wait() -> int
- wait_for(ms: int) -> bool
- wait_until(deadline_ms: int) -> bool
- poll() -> bool
- has_exited() -> bool
- get_pid() -> int
- kill(force: bool = true)
- kill_tree(force: bool = true)
- communicate() -> [out: str/null, err: str/null, exit_code: int]

kill_tree 语义：

- Windows: 终止整个 Job 对象或等价进程树。
- Unix: 终止进程组（setpgid/killpg 语义）。
- 若目标平台无法保证树级终止，必须在文档和运行时给出明确告警。

## 3. 统一语义约束

- 不暴露 send_signal。
- shell 通过 `shell(program)` 启用，通过 `shell_off()` 关闭。
- 脚本侧请使用 `import process`，避免与解释器内置 `process` 冲突。
- merge_output=true 时，redirect_err 无效。
- inherit_output=true 时不采集 out/err，communicate 的 out/err 为 null。
- 参数优先级：merge_output > redirect_out/redirect_err > inherit_output。
- redirect_out/redirect_err 仅接受 file_t；通过 clear_redirect_out/clear_redirect_err 清除。
- 参数类型错误抛异常；运行时失败使用 null/false 返回值表达。
- wait_for/wait_until 超时返回 false，不抛异常。
- wait_with 的 callback 若抛异常，应立即中断 wait_with 并向上抛出；子进程保持原生运行状态，不做隐式 kill。
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
- builder.shell 只保留 null|string 签名。
- 增加 process.shell(command, options) 与 get_pid。

### Phase 2: mpp 收口

- process_builder 删除 shell(bool)/shell_command(str) 公开语义。
- start() 仅保留 shell=null 与 shell=str 两条执行路径。

### Phase 3: libuv 与异步文件 I/O

- 基于 libuv 接入统一事件循环与调度机制（外露基础 loop API：poll/poll_once/restart/stop）。
- mpp 引入 file_t 异步文件抽象。
- redirect_out/redirect_err 完成到 file_t 的绑定迁移。

### Phase 4: 增强能力

- wait/communicate 全量接入 deadline 语义。
- stdin 异步写入队列与背压。
- kill_tree 进程树终止。
- 基础可观测事件（spawn/read/write/wait/kill/timeout）。

## 5. 验收标准

- 现有 test_unit.csc 全量通过。
- 大输出 communicate 无死锁、无截断。
- wait_poll/wait_with 高频场景 CPU 不高于当前版本。
- Windows 与 Unix 构建通过。
- file_t 重定向场景下，process 销毁后文件对象仍可按其自身语义安全关闭。
