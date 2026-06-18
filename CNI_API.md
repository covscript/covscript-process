# CovScript Process Extension — CNI API

CovScript 脚本层接口文档。C++ 层 API 见 [CXX_API.md](CXX_API.md)。

## 决策摘要

- 异步后端统一采用 libuv。
- 旧接口与兼容层全部删除，不保留 legacy 后端。
- redirect_in/redirect_out/redirect_err 只接受 file_t 对象，不接受路径字符串。
- 不新增 process 专用"按块读取"接口，统一复用异步文件 I/O 能力。
- `mpp` C++ 层与脚本 CNI 层不要求 1:1 对齐；CNI 仅暴露脚本侧需要且稳定的子集。

解释器基线：

- 当前解释器 ABI：`COVSCRIPT_ABI_VERSION = 260602`（v3.4.8）。
- 扩展内部 fiber 协作门槛 ABI：`>= 250908`，在当前解释器上默认走 fiber 兼容路径。

---

## 1. 事件循环

`process.async` 命名空间。

| 函数 | 签名 | 说明 |
|------|------|------|
| `poll` | `() -> int` | 运行直到当前队列空闲，返回本轮处理的事件数量 |
| `poll_once` | `() -> bool` | 仅推进一次事件分发，返回是否处理了事件 |
| `stop` | `()` | 停止事件循环 |
| `restart` | `()` | stop 后重新进入可运行状态（当前为 no-op） |

说明：

- 同一个 loop 只能由一个线程驱动；并发调用 poll/poll_once 属于未定义行为。
- `restart()` 为语义对称接口；当前实现是 no-op（libuv 在 stop 后可直接继续 uv_run）。

## 2. 异步文件 I/O

### 2.1 打开文件

```
process.async.fstream(path: str, mode: str) -> file_t | null
```

| mode | 说明 |
|------|------|
| `"r"` | 只读，文件必须存在 |
| `"w"` | 只写，创建/截断 |
| `"a"` | 追加，不存在则创建 |
| `"r+"` | 读写，文件必须存在 |
| `"w+"` | 读写，创建/截断 |

- 不可识别的 mode 抛异常；文件不存在或无权限返回 null。

### 2.2 file_t 方法

| 方法 | 签名 | 说明 |
|------|------|------|
| `read` | `(size: int, deadline_ms: int = -1) -> str \| null` | 读取最多 size 字节。空字符串 = EOF，null = 错误/超时/已关闭 |
| `write` | `(data: str, deadline_ms: int = -1) -> int` | 写入数据，返回实际字节数，-1 = 错误 |
| `flush` | `(deadline_ms: int = -1) -> bool` | 刷盘，返回是否在 deadline 内完成 |
| `close` | `()` | 关闭文件（幂等） |
| `is_readable` | `() -> bool` | 文件是否可读且未关闭 |
| `is_writable` | `() -> bool` | 文件是否可写且未关闭 |
| `in` | `() -> ostream \| null` | 写入流，不可写或已关闭时返回 null |
| `out` | `() -> istream \| null` | 读取流，不可读或已关闭时返回 null |

说明：

- file_t 是重定向唯一绑定对象。
- 文件对象持有底层句柄生命周期，process 不再额外托管该状态。
- `deadline_ms < 0` 表示无限等待；`>= 0` 表示超时毫秒数。
- `deadline` 语义由 libuv 请求取消驱动；超时后返回失败值，不保证底层 OS I/O 立刻终止。
- append 模式 (`"a"`) 下 `write` 始终追加到文件末尾，不受 `write_pos` 影响。

## 3. 进程启动

| 函数 | 签名 | 说明 |
|------|------|------|
| `process.exec` | `(executable: str, args: array) -> process_t` | 直接启动可执行文件 |
| `process.shell` | `(command: str) -> process_t` | 通过平台 shell 启动（`cmd /c` / `sh -c`） |
| `new process.builder` + `.start()` | 见下文 | Builder 模式，灵活配置 |

## 4. process.builder

通过 `new process.builder` 创建。

| 方法 | 签名 | 说明 |
|------|------|------|
| `cmd` | `(value: str)` | 设置可执行文件（argv[0]） |
| `arg` | `(values: array)` | 设置参数列表（最多调用一次） |
| `dir` | `(value: str)` | 设置工作目录 |
| `env` | `(key: str, value: str)` | 设置环境变量（可多次调用） |
| `inherit_env` | `(value: bool)` | 是否继承父进程环境（默认 true） |
| `inherit_output` | `(value: bool)` | 是否继承父进程 stdout/stderr |
| `merge_output` | `(value: bool)` | 是否将 stderr 合并到 stdout |
| `use_shell` | `(program: str)` | 使用 shell 模式启动 |
| `redirect_in` | `(file: file_t)` | 重定向 stdin 从 file_t 读取 |
| `redirect_out` | `(file: file_t)` | 重定向 stdout 写入 file_t |
| `redirect_err` | `(file: file_t)` | 重定向 stderr 写入 file_t |
| `start` | `() -> process_t` | 启动进程 |

## 5. process_t

进程运行句柄。

### 5.1 流访问

| 方法 | 返回 | 说明 |
|------|------|------|
| `in()` | `ostream` | 子进程 stdin 写入流 |
| `out()` | `istream` | 子进程 stdout 读取流 |
| `err()` | `istream` | 子进程 stderr 读取流 |

### 5.2 等待

| 方法 | 签名 | 说明 |
|------|------|------|
| `wait` | `() -> int` | 阻塞等待退出，返回 exit_code |
| `try_wait` | `() -> int \| null` | 非阻塞检查，已退出返回 exit_code，否则 null |
| `wait_poll` | `(timeout_ms: int, poll_interval_ms: int = 5) -> int \| null` | 轮询等待，超时返回 null |
| `wait_with` | `(timeout_ms: int, callback: callable) -> int \| null` | 带回调的轮询等待 |

### 5.3 状态

| 方法 | 签名 | 说明 |
|------|------|------|
| `has_exited` | `() -> bool` | 进程是否已退出 |
| `is_running` | `() -> bool` | 进程是否仍在运行 |
| `get_pid` | `() -> int` | 返回 OS 进程 ID |

### 5.4 控制

| 方法 | 签名 | 说明 |
|------|------|------|
| `kill` | `(force: bool = true)` | 终止进程 |
| `kill_tree` | `(force: bool = true)` | 终止进程及其所有子进程 |

### 5.5 通信

```
communicate() -> [out: str, err: str, exit_code: int]
```

- 同时排空 stdout 和 stderr（避免管道满死锁），等待进程退出，返回三元组。
- inherit_output=true 时 out/err 为空字符串。
- 在 fiber 上下文中自动使用协作式 yield。

---

## 6. 语义约束

### 参数优先级

```
merge_output > redirect_out/redirect_err > inherit_output
```

### 退出码约定

| 场景 | exit_code |
|------|-----------|
| 正常退出 | 原始退出码（0..255） |
| SIGTERM (kill(false)) | 143（128 + 15） |
| SIGKILL (kill(true)) | 137（128 + 9） |
| Windows kill(force=false) | 143 |
| Windows kill(force=true) | 137 |
| Windows 其他外部终止 | 143（按 SIGTERM 语义） |

### 其他约束

- 不暴露 `send_signal`。
- `merge_output=true` 时，`redirect_err` 无效。
- `inherit_output=true` 时不采集 out/err，communicate 的 out/err 为空字符串。
- redirect_in/redirect_out/redirect_err 仅接受 file_t。
- 参数类型错误抛异常；运行时失败使用 null/false 返回值表达。
- `wait_poll` 超时返回 null，不抛异常。
- `wait_with` 的 callback 若抛异常，立即中断 wait_with 并向上抛出；子进程保持运行，不做隐式 kill。

---

## 7. 验收标准

- 现有测试（./tests）全量通过。
- 保证测试能够覆盖全部 CNI 接口。
- 大输出 communicate 无死锁、无截断。
- wait_poll/wait_with 高频场景 CPU 不高于当前版本。
- Windows 与 Unix 构建通过。
- file_t 重定向场景下，process 销毁后文件对象仍可按其自身语义安全关闭。
