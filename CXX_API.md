# CovScript Process Extension — C++ API (Mozart++)

C++ mpp 库层接口文档。脚本层 CNI API 见 [CNI_API.md](CNI_API.md)。

**命名空间**：`mpp`（公共类型）、`mpp_impl`（平台内部实现）。

**头文件**：`#include <mozart++/process>`、`#include <mozart++/file>`

---

## 1. mpp::file

```cpp
#include <mozart++/file>
```

跨平台文件句柄封装。Win32 使用 `HANDLE`，Unix 使用 fd。

### 1.1 状态查询

| 方法 | 签名 | 说明 |
|------|------|------|
| `is_readable` | `() const -> bool` | 文件是否可读且未关闭 |
| `is_writable` | `() const -> bool` | 文件是否可写且未关闭 |
| `is_closed` | `() const -> bool` | 文件是否已关闭 |
| `is_append` | `() const -> bool` | 是否以 append 模式打开 |

### 1.2 偏移量管理

CNI 异步读写使用的位置追踪。非 append 模式下 `write()` 使用 `write_position()` 作为偏移量；append 模式下由 OS 管理。

| 方法 | 签名 | 说明 |
|------|------|------|
| `read_position` | `() const -> int64_t` | 当前读位置 |
| `advance_read` | `(int64_t n)` | 读位置前进 n 字节 |
| `write_position` | `() const -> int64_t` | 当前写位置 |
| `advance_write` | `(int64_t n)` | 写位置前进 n 字节 |

### 1.3 原生句柄

| 方法 | 说明 |
|------|------|
| `native_fd()` | Windows: 返回 `HANDLE`；Unix: 返回 `int fd` |
| `get_uv_fd()` | （仅 Windows）返回 `_open_osfhandle` 创建的 C 运行时 fd |

### 1.4 生命周期

| 方法 | 签名 | 说明 |
|------|------|------|
| `close_file` | `()` | 关闭文件（幂等）。析构函数自动调用 |

### 1.5 流包装（懒创建）

| 方法 | 返回 | 说明 |
|------|------|------|
| `out_stream` | `fdistream&` | 读取流。`close_file()` 后失效 |
| `in_stream` | `fdostream&` | 写入流。`close_file()` 后失效 |

### 1.6 工厂函数

```cpp
mpp::file_ptr mpp::open_file(const std::string &path, const std::string &mode);
```

| mode | 说明 |
|------|------|
| `"r"` | 只读，文件必须存在 |
| `"w"` | 只写，创建/截断 |
| `"a"` | 追加，不存在则创建 |
| `"r+"` | 读写，文件必须存在 |
| `"w+"` | 读写，创建/截断 |
| `"a+"` | 读写（追加），不存在则创建 |

- 返回 `std::shared_ptr<mpp::file>`，失败返回 nullptr。
- 不可识别的 mode 抛 `mpp::runtime_error`。
- Windows 上文件句柄以 inheritable 方式打开，可直接用于 process redirect。

### 1.7 设计说明

- 所有数据成员为 private，通过方法访问。
- 不可拷贝、不可移动（持有 OS 句柄的所有权）。
- `read_at()`/`write_bytes()`/`flush_file()` 等同步方法已移除；CNI 层统一使用 libuv `uv_fs_*` 异步 I/O。

---

## 2. mpp::process

```cpp
#include <mozart++/process>
```

子进程运行句柄。不可拷贝，可移动（构造 + 赋值）。

### 2.1 流访问

从**调用者视角**命名：`in()` 是写入子进程 stdin 的端，`out()`/`err()` 是读取子进程 stdout/stderr 的端。

| 方法 | 返回 | 说明 |
|------|------|------|
| `in()` | `std::ostream&` | 子进程 stdin **写端** |
| `out()` | `std::istream&` | 子进程 stdout **读端** |
| `err()` | `std::istream&` | 子进程 stderr **读端** |

### 2.2 stdin 控制

| 方法 | 签名 | 说明 |
|------|------|------|
| `close_stdin` | `()` | 关闭 stdin 写端，向子进程发送 EOF（幂等） |

- `communicate()` 会自动调用 `close_stdin()`。
- 手动调用后，`in()` 流不再可用。

### 2.3 等待

| 方法 | 签名 | 说明 |
|------|------|------|
| `wait_timeout_ms` | `(int timeout_ms, int poll_interval_ms = 5) -> std::optional<int>` | 带超时等待。nullopt = 超时 |
| `begin_wait` | `()` | 提交阻塞 wait 到 libuv 线程池（幂等） |
| `poll_wait` | `() -> bool` | 非阻塞检查，true = 已退出 |
| `collect_wait` | `() -> int` | 阻塞收集结果（驱动 uv_run）。未调 begin_wait 时回退到同步等待 |

典型异步用法：

```cpp
p->begin_wait();
while (!p->poll_wait())
    // yield or sleep
    ;
int code = p->collect_wait();
```

### 2.4 状态与控制

| 方法 | 签名 | 说明 |
|------|------|------|
| `has_exited` | `() -> bool` | 进程是否已退出 |
| `interrupt` | `(bool force = false)` | 终止进程 |
| `interrupt_tree` | `(bool force = false)` | 终止进程树 |
| `pid` | `() const -> int` | 返回 OS 进程 ID |

### 2.5 communicate

```cpp
struct communicate_result {
    std::string out;
    std::string err;
    int exit_code = 0;
};
```

| 方法 | 签名 | 说明 |
|------|------|------|
| `begin_communicate` | `()` | 关闭 stdin + 提交 stdout/stderr 读取 + 等待到线程池 |
| `poll_communicate` | `() -> bool` | 非阻塞检查，true = 读取完成 |
| `end_communicate` | `() -> communicate_result` | 收集结果并等待退出 |
| `communicate` | `() -> communicate_result` | 阻塞便捷方法（begin + 等待 + collect） |

**关键行为**：`begin_communicate()` 会自动关闭 stdin 写端，让从 stdin 读取的子进程能看到 EOF 并正常退出。

### 2.6 静态工厂

```cpp
static process exec(const std::string &command);
static process exec(const std::string &command, const std::vector<std::string> &args);
```

### 2.7 移动语义

```cpp
process(process &&) = default;                    // 可移动构造
process &operator=(process &&other) noexcept;     // 可移动赋值
process(const process &) = delete;                // 不可拷贝
process &operator=(const process &) = delete;     // 不可拷贝
```

---

## 3. mpp::process_builder

```cpp
#include <mozart++/process>
```

Builder 模式配置进程启动参数。所有 setter 返回 `process_builder&` 以支持链式调用。可拷贝、可移动。

### 3.1 方法

| 方法 | 签名 | 说明 |
|------|------|------|
| `command` | `(const std::string&) -> process_builder&` | 设置 argv[0]，可多次调用（覆盖） |
| `arguments` | `(const Container&) -> process_builder&` | 设置参数列表（可多次调用，后写覆盖） |
| `environment` | `(const std::string& key, const std::string& value) -> process_builder&` | 设置环境变量。同名 key 后写覆盖前写（last-write-wins） |
| `directory` | `(const std::string& cwd) -> process_builder&` | 设置工作目录 |
| `merge_outputs` | `(bool) -> process_builder&` | 合并 stderr 到 stdout |
| `inherit_stdin` | `(bool = true) -> process_builder&` | 继承父进程 stdin |
| `inherit_output` | `(bool = true) -> process_builder&` | 继承父进程 stdout+stderr |
| `inherit_env` | `(bool = true) -> process_builder&` | 继承父进程环境 |
| `redirect_stdin` | `(fd_type) -> process_builder&` | 重定向 stdin |
| `redirect_stdout` | `(fd_type) -> process_builder&` | 重定向 stdout |
| `redirect_stderr` | `(fd_type) -> process_builder&` | 重定向 stderr |
| `shell` | `(const std::string&) -> process_builder&` | 设置 shell 模式 |
| `shell` | `(std::nullptr_t) -> process_builder&` | 关闭 shell 模式 |
| `start` | `() -> process` | 启动进程 |

### 3.2 Shell 模式

- `shell(program)` 设置 shell 程序（如 `"/bin/sh"` 或 `"cmd"`）。
- `shell(nullptr)` 关闭 shell 模式。
- shell 模式下 `start()` 将命令行包装为 `{shell, "-c"/"/c", combined_cmd}`。

### 3.3 arguments() 约束

- `arguments()` 可以多次调用，后调用的值覆盖之前的参数（last-wins）。
- 每次调用会先清除旧的参数再追加新的，不依赖 `_cmdline.size()` 判断是否为首次调用。

---

## 4. mpp_impl 平台接口

平台特定实现，不直接暴露给脚本。由 `mpp::process` 内部调用。

### 4.1 process_startup

```cpp
struct process_startup {
    std::vector<std::string> _cmdline;
    std::optional<std::string> _shell_program;
    std::unordered_map<std::string, std::string> _env;
    bool _inherit_env = true;
    std::string _cwd = ".";
    redirect_info _stdin, _stdout, _stderr;
    bool merge_outputs = false;
    bool inherit_stdin = false;
    bool inherit_stdout = false;
    bool inherit_stderr = false;
    bool shell_mode = false;
};
```

### 4.2 process_info

```cpp
struct process_info {
    fd_type _tid;           // 线程句柄（仅 Windows）
    fd_type _pid;           // 进程句柄（Windows）或 PID（Unix）
    fd_type _stdin;         // stdin 管道写端
    fd_type _stdout;        // stdout 管道读端
    fd_type _stderr;        // stderr 管道读端
    bool _stdin_closed;     // stdin 是否已关闭
};
```

### 4.3 平台函数

| 函数 | 签名 | 说明 |
|------|------|------|
| `create_process` | `(startup, info)` | 创建进程（设置管道/重定向） |
| `close_process` | `(info)` | 关闭进程管道 |
| `wait_for` | `(info) -> int` | 阻塞等待退出 |
| `terminate_process` | `(info, force)` | 终止进程 |
| `terminate_process_tree` | `(info, force)` | 终止进程树 |
| `process_exited` | `(info) -> bool` | 非阻塞检查是否退出 |
| `wait_timeout_ms` | `(info, timeout_ms, exit_code&, poll_interval_ms) -> bool` | 带超时等待。`timeout_ms < 0` 视为无限等待；`timeout_ms = 0` 仅探测一次；`timeout_ms > 0` 正常超时 |
| `get_pid` | `(info) -> int` | 获取进程 ID |

### 4.4 平台实现差异

| 特性 | Windows | Unix |
|------|---------|------|
| 进程创建 | `CreateProcess` + `STARTUPINFO` | `fork` + `execvpe` |
| 等待 | `WaitForSingleObject` | `waitid(P_PID)` |
| 非阻塞检查 | `WaitForSingleObject(0)` | `waitid(WNOHANG\|WNOWAIT)` |
| 超时等待 | `WaitForSingleObject(timeout)` | 轮询 `nanosleep` + `waitid` |
| 进程树终止 | `CreateToolhelp32Snapshot` 枚举子进程 | `kill(-pgid)`（同一进程组）；`terminate_process_tree` 在发送进程组信号前通过 `_start_time` 校验进程身份，防止 PID 复用误杀 |
| 环境变量 | `GetEnvironmentStrings` + `CreateProcess` | `environ` + `fork` 前构建 |
| 命令行引号 | MSVCRT 规则（反斜杠-引号双写） | 无特殊处理 |
| fd 清理 | N/A（句柄继承控制） | `close_range(2)` (Linux) / `/dev/fd` (macOS) / brute-force |
| 进程身份校验 | `GetProcessTimes` 记录 `_start_time` | `/proc/<pid>/stat` (Linux) / `sysctl KERN_PROC_PID` (macOS) 记录 `_start_time`

---

## 5. 辅助类型

### 5.1 fdostream / fdistream

```cpp
#include <mozart++/fdstream>
```

`std::streambuf` 实现，包装原生 fd。由 `mpp::file::in_stream()` / `out_stream()` 和 `mpp::process::in()` / `out()` / `err()` 内部使用。

---

## 6. 构建说明

```cmake
# 需要环境变量 CS_DEV_PATH 指向 CovScript SDK
# C++17 标准（由 SDK csbuild.cmake 设置）
# 构建产物：
#   mozart    — 静态库（mpp C++ 层）
#   process   — 共享库 .cse（CNI 绑定层）
```

### Windows (MinGW)

```powershell
cd cmake-build/mingw-w64
cmake -G "MinGW Makefiles" ../..
mingw32-make -j4
```

### Linux / macOS

```bash
mkdir -p cmake-build/unix && cd cmake-build/unix
cmake -G "Unix Makefiles" ../..
make -j4
```
