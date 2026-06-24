# CovScript Process Extension — CNI API

CovScript 脚本层接口文档。C++ 层 API 见 [CXX_API.md](CXX_API.md)。

## 快速开始

```covscript
import process

# 一行启动，等待完成
var r = process.shell("echo hello").communicate()
# r == ["hello\n", "", 0]   (stdout, stderr, exit_code)

# Builder 模式：链式配置
var p = new process.builder
p.cmd("echo").arg({"hello", "world"}).shell("cmd").merge_output(true)
var r2 = p.start().communicate()
```

---

## 兼容性

Process Extension 的 CNI 接口**完全向后兼容**——旧有接口全部保留，签名与语义不变；仅新增能力。

| 类别 | Legacy 接口 | Modern 新增 |
|------|-------------|-------------|
| 顶层启动 | `process.exec(cmd, args)` | `process.shell(command)` |
| builder 配置 | `cmd`, `arg`, `dir`, `env`, `merge_output`, `start` | `shell`, `inherit_output`, `inherit_env`, `redirect_in`, `redirect_out`, `redirect_err` |
| 进程等待 | `wait`, `has_exited` | `try_wait`, `wait_poll`, `wait_with`, `is_running` |
| 进程控制 | `kill` | `kill_tree`, `get_pid` |
| 进程通信 | `in`, `out`, `err` | `communicate` |
| 文件 I/O | — | `file_t` + `process.async.fstream` + 事件循环 |
| 异步事件 | — | `process.async.poll`, `poll_once`, `stop`, `restart` |

### 行为差异

#### builder 方法链

所有 builder 配置方法（`cmd`, `arg`, `dir`, `env`, `merge_output`, `shell`, `inherit_output`, `inherit_env`, `redirect_in`, `redirect_out`, `redirect_err`）均返回 builder 自身，支持链式调用。`start()` 返回 `process_t`。

#### `arg()` 重复调用

Legacy 接口静默忽略第二次及后续调用；Modern 接口抛出异常。重复 `arg()` 在原实现中即属非法操作。

#### `process_t.wait()`

| 项目 | Legacy 接口 | Modern 接口 |
|------|--------|---------------|
| **普通上下文** | `p->wait_for()` → 直接阻塞 OS 线程 | `p->collect_wait()` → 最终调用同一底层函数 |
| **协程上下文** | 阻塞 OS 线程，阻止同线程其他 fiber 调度 | libuv 线程池 + `cs::fiber::yield()` 协作让步 |
| **返回值** | `int` 退出码 | `int` 退出码 |
| **ECHILD 处理** | 返回 0 | 返回 0 |

#### `process_t.has_exited()`

| 项目 | Legacy 接口 | Modern 接口 |
|------|--------|---------------|
| **OS 查询** | `mpp_impl::process_exited()` | 同，增加缓存：首次返回 `true` 后不再查询 OS |
| **ECHILD 回退** | 无 | 有——若子进程被外部收割，通过 `/proc` 或 `kill(0)` 确认 |

#### `process_t.kill(force)`

| 项目 | Legacy 接口 | Modern 接口 |
|------|--------|---------------|
| **Unix `force=true`** | `kill(SIGKILL)` → 退出码 137 | 同 |
| **Unix `force=false`** | `kill(SIGTERM)` → 退出码 143 | 同 |
| **Windows** | `TerminateProcess`，忽略 `force` 参数，始终硬杀 | `force=true` → 137，`force=false` → 143 |
| **进程树** | 不支持 | 新增 `kill_tree(force)` |

> ⚠️ Windows 上 `kill(false)` 行为改善：Legacy 接口 `force=false` 和 `force=true` 无区别（均硬杀）；Modern 接口退出码正确反映语义。依赖旧行为的脚本请使用退出码约定（见 §语义约束）而非硬编码数值。

---

## 1. 进程启动

### 1.1 快捷函数

| 函数 | 签名 | 说明 |
|------|------|------|
| `process.exec` | `(executable: str, args: array) -> process_t` | 直接启动可执行文件（`argv[0]` + `argv[1..]`） |
| `process.shell` | `(command: str) -> process_t` | 通过平台 shell 启动（Windows: `cmd /c`，Unix: `sh -c`） |

### 1.2 process.builder

通过 `new process.builder` 创建，所有配置方法返回 builder 自身（链式调用），`start()` 返回 `process_t`。

| 方法 | 签名 | 说明 |
|------|------|------|
| `cmd` | `(value: str)` | 设置可执行文件路径 |
| `arg` | `(values: array)` | 设置参数列表（最多调用一次，重复调用抛异常） |
| `dir` | `(value: str)` | 设置工作目录 |
| `env` | `(key: str, value: str)` | 设置环境变量（可多次调用，逐次累积） |
| `inherit_env` | `(value: bool)` | 是否继承父进程环境（默认 true） |
| `inherit_output` | `(value: bool)` | 子进程 stdout/stderr 复用父进程终端 |
| `merge_output` | `(value: bool)` | stderr 合并到 stdout |
| `shell` | `(program: str)` | 启用 shell 模式，传入 shell 程序路径（如 `"cmd"` 或 `"/bin/sh"`） |
| `redirect_in` | `(file: file_t)` | 子进程 stdin 从 file_t 读取 |
| `redirect_out` | `(file: file_t)` | 子进程 stdout 写入 file_t |
| `redirect_err` | `(file: file_t)` | 子进程 stderr 写入 file_t |
| `start` | `() -> process_t` | 启动进程 |

示例：

```covscript
var b = new process.builder
b.cmd("echo").arg({"hello"}).shell("cmd").merge_output(true)
var p = b.start()
var r = p.communicate()  # [stdout: str, stderr: str, exit_code: int]
```

---

## 2. process_t

进程运行句柄，由 `process.exec` / `process.shell` / `builder.start()` 返回。

### 2.1 流访问

| 方法 | 返回 | 说明 |
|------|------|------|
| `in()` | `ostream` | 子进程 stdin 写入流 |
| `out()` | `istream` | 子进程 stdout 读取流 |
| `err()` | `istream` | 子进程 stderr 读取流 |

### 2.2 等待

| 方法 | 签名 | 说明 |
|------|------|------|
| `wait` | `() -> int` | 阻塞等待退出，返回 exit_code |
| `try_wait` | `() -> int \| null` | 非阻塞检查；已退出返回 exit_code，否则返回 null。需要先通过 `wait_poll` / `wait_with` / `wait`（fiber 路径）触发内部异步等待，否则可能退化为单次 OS 查询 |
| `wait_poll` | `(timeout_ms: int, poll_interval_ms: int) -> int \| null` | 轮询等待，超时返回 null。`poll_interval_ms` 会被 clamp 到最小 1ms。`timeout_ms < 0` 表示无限等待 |
| `wait_with` | `(timeout_ms: int, callback: callable) -> int \| null` | 带回调的轮询等待，每轮迭代调用 `callback()` 替代 sleep/yield。`timeout_ms < 0` 无限轮询。callback 若抛异常则立即向上抛出，不隐式 kill 子进程 |

### 2.3 状态

| 方法 | 签名 | 说明 |
|------|------|------|
| `has_exited` | `() -> bool` | 进程是否已退出（含 ECHILD 回退） |
| `is_running` | `() -> bool` | 进程是否仍在运行 |
| `get_pid` | `() -> int` | 返回 OS 进程 ID |

### 2.4 控制

| 方法 | 签名 | 说明 |
|------|------|------|
| `kill` | `(force: bool)` | 终止进程。`force=true` → SIGKILL（退出码 137），`force=false` → SIGTERM（退出码 143） |
| `kill_tree` | `(force: bool)` | 终止进程及其所有子进程。对已退出进程调用不报错 |

### 2.5 通信

```
communicate() -> [out: str, err: str, exit_code: int]
```

- 并行排空 stdout 和 stderr（避免管道满死锁），等待进程退出，返回三元组。
- `inherit_output=true` 时 out/err 为空字符串。
- `merge_output=true` 时 err 为空字符串（stderr 已合并到 stdout）。
- 在 fiber 上下文中自动使用协作式 yield。

---

## 3. 事件循环

`process.async` 命名空间。基于 libuv 的默认循环（`uv_default_loop()`）。

| 函数 | 签名 | 说明 |
|------|------|------|
| `poll` | `() -> int` | 驱动事件循环（非阻塞）。返回 0 表示循环无活跃句柄/请求，非零表示仍有活跃句柄/请求 |
| `poll_once` | `() -> bool` | 运行一次事件循环迭代（可能短暂阻塞等待 I/O）。返回 true 表示仍有未完成的工作 |
| `stop` | `()` | 停止事件循环（`uv_stop`） |
| `restart` | `()` | stop 后重新进入可运行状态。当前为 no-op（libuv 在 stop 后可直接继续 `uv_run`） |

- 同一个 loop 只能由一个线程驱动；并发调用 poll / poll_once 属于未定义行为。

---

## 4. 异步文件 I/O

### 4.1 打开文件

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

- 不可识别的 mode 抛出 **native 异常**（`mpp::runtime_error`），该异常**不可被 CovScript `try/catch` 捕获**，会导致脚本终止。请在调用侧确保 mode 合法。
- 文件不存在或无权限返回 null。

### 4.2 file_t 方法

| 方法 | 签名 | 说明 |
|------|------|------|
| `read` | `(size: int, deadline_ms: int) -> str \| null` | 读取最多 size 字节。空字符串 = EOF 或 size ≤ 0，null = 错误/超时/已关闭。`deadline_ms < 0` 无限等待 |
| `write` | `(data: str, deadline_ms: int) -> int` | 写入数据，返回实际字节数，-1 = 错误。append 模式下始终追加到文件末尾。`deadline_ms < 0` 无限等待 |
| `flush` | `(deadline_ms: int) -> bool` | 刷盘，返回是否在 deadline 内完成。`deadline_ms < 0` 无限等待 |
| `close` | `()` | 关闭文件（幂等，可安全重复调用） |
| `is_readable` | `() -> bool` | 文件是否可读且未关闭 |
| `is_writable` | `() -> bool` | 文件是否可写且未关闭 |
| `in` | `() -> ostream \| null` | 写入流，不可写或已关闭时返回 null |
| `out` | `() -> istream \| null` | 读取流，不可读或已关闭时返回 null |

- file_t 持有底层句柄生命周期，process 不额外托管该状态。
- deadline 语义由 libuv 请求取消驱动；超时后返回失败值，不保证底层 OS I/O 立刻终止。
- process 销毁后，file_t 对象仍可按其自身语义安全关闭。

---

## 5. 语义约束

### 参数优先级

当 `inherit_output`、`redirect_out`/`redirect_err`、`merge_output` 同时设置时，实际生效规则：

- **stdout**：`inherit_output` 优先于 `redirect_out`（设置 inherit 后 redirect 被忽略）
- **stderr**：`merge_output` 优先于 `inherit_output`（inherit_stdout），优先于 `redirect_err`

简言之：`merge_output > inherit_output > redirect_out / redirect_err`

### 退出码约定

| 场景 | exit_code |
|------|-----------|
| 正常退出 | 原始退出码（0..255） |
| SIGTERM (`kill(false)`) | 143（128 + 15） |
| SIGKILL (`kill(true)`) | 137（128 + 9） |
| Windows `kill(false)` | 143 |
| Windows `kill(true)` | 137 |
| Windows 其他外部终止 | 143（按 SIGTERM 语义） |

### 其他约束

- 不暴露 `send_signal`。
- `merge_output=true` 时，`redirect_err` 无效。
- `inherit_output=true` 时不采集 out/err，communicate 的 out/err 为空字符串。
- `redirect_in` / `redirect_out` / `redirect_err` 仅接受 file_t，不接受路径字符串。
- 参数类型错误抛异常；运行时失败使用 null / false 返回值表达。
- `wait_poll` 超时返回 null，不抛异常。
- `wait_with` 的 callback 若抛异常，立即中断 wait_with 并向上抛出；子进程保持运行，不做隐式 kill。

---

## 6. 验收标准

- 现有测试（`./tests`）全量通过。
- 保证测试能够覆盖全部 CNI 接口。
- 大输出 communicate 无死锁、无截断。
- wait_poll / wait_with 高频场景 CPU 不高于当前版本。
- Windows 与 Unix 构建通过。
- file_t 重定向场景下，process 销毁后文件对象仍可按其自身语义安全关闭。
