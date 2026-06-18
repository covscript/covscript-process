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

### 1.1 公开成员

| 成员 | 类型 | 说明 |
|------|------|------|
| `readable` | `bool` | 文件是否可读 |
| `writable` | `bool` | 文件是否可写 |
| `closed` | `bool` | 文件是否已关闭 |
| `append` | `bool` | 是否以 append 模式打开 |
| `read_pos` | `int64_t` | 当前读位置（由 read_at / CNI read 更新） |
| `write_pos` | `int64_t` | 当前写位置（由 CNI write 更新，append 模式不使用） |

### 1.2 平台相关

**Windows**：

| 成员/方法 | 说明 |
|-----------|------|
| `handle` | `HANDLE`，Win32 原生句柄 |
| `uv_fd` | `int`，`_open_osfhandle` 创建的 C 运行时 fd，供 libuv 使用 |
| `native_fd()` | 返回 `handle` |
| `get_uv_fd()` | 返回 `uv_fd` |

**Unix**：

| 成员/方法 | 说明 |
|-----------|------|
| `fd` | `int`，POSIX 文件描述符 |
| `native_fd()` | 返回 `fd` |

### 1.3 方法

| 方法 | 签名 | 说明 |
|------|------|------|
| `close_file` | `()` | 关闭文件（幂等）。析构函数自动调用 |
| `read_at` | `(char *buf, int size) -> int` | 在 `read_pos` 位置读取，返回字节数 / 0(EOF) / -1(错误) |
| `write_bytes` | `(const char *buf, int size) -> int` | 写入数据，返回字节数或 -1 |
| `flush_file` | `() -> bool` | 刷盘 |
| `out_stream` | `() -> fdistream&` | 懒创建的读取流包装 |
| `in_stream` | `() -> fdostream&` | 懒创建的写入流包装 |

### 1.4 工厂函数

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

- 返回 `std::shared_ptr<mpp::file>`，失败返回 nullptr。
- 不可识别的 mode 抛 `mpp::runtime_error`。
- Windows 上文件句柄以 inheritable 方式打开，可直接用于 process redirect。

### 1.5 类型别名

```cpp
using file_ptr = std::shared_ptr<file>;
```

---

## 2. mpp::process

```cpp
#include <mozart++/process>
```

子进程运行句柄。不可拷贝，可移动。

### 2.1 流访问

| 方法 | 返回 | 说明 |
|------|------|------|
| `in()` | `std::ostream&` | 子进程 stdin 写入流 |
| `out()` | `std::istream&` | 子进程 stdout 读取流 |
| `err()` | `std::istream&` | 子进程 stderr 读取流 |

### 2.2 同步等待

| 方法 | 签名 | 说明 |
|------|------|------|
| `wait_for` | `() -> int` | 阻塞等待退出（缓存结果） |
| `wait_timeout_ms` | `(int timeout_ms, int poll_interval_ms = 5) -> std::optional<int>` | 带超时的等待，nullopt = 超时 |

### 2.3 异步等待（libuv 线程池）

| 方法 | 签名 | 说明 |
|------|------|------|
| `begin_wait` | `()` | 提交阻塞 wait 到 libuv 线程池（幂等） |
| `poll_wait` | `() -> bool` | 非阻塞检查，true = 已退出 |
| `collect_wait` | `() -> int` | 阻塞收集结果（驱动 uv_run） |

典型用法：

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
| `has_exited` | `() -> bool` | 进程是否已退出（驱动 uv_run + OS 检查） |
| `interrupt` | `(bool force = false)` | 终止进程（force=true → SIGKILL / TerminateProcess(137)） |
| `interrupt_tree` | `(bool force = false)` | 终止进程树 |
| `pid` | `() -> int` | 返回 OS 进程 ID |

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
| `begin_communicate` | `()` | 提交 stdout/stderr 读取 + 等待到线程池 |
| `poll_communicate` | `() -> bool` | 非阻塞检查，true = 读取完成 |
| `end_communicate` | `() -> communicate_result` | 收集结果并等待退出 |
| `communicate` | `() -> communicate_result` | 阻塞便捷方法（begin + 等待 + collect） |

### 2.6 静态工厂

```cpp
static process exec(const std::string &command);
static process exec(const std::string &command, const std::vector<std::string> &args);
```

---

## 3. mpp::process_builder

```cpp
#include <mozart++/process>
```

Builder 模式配置进程启动参数。所有 setter 返回 `process_builder&` 以支持链式调用。

### 3.1 方法

| 方法 | 签名 | 说明 |
|------|------|------|
| `command` | `(const std::string&) -> process_builder&` | 设置 argv[0]，可多次调用（覆盖） |
| `arguments` | `(const Container&) -> process_builder&` | 设置参数列表（最多调用一次，否则抛异常） |
| `environment` | `(const std::string& key, const std::string& value) -> process_builder&` | 设置环境变量 |
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
- C++ 层保留 `shell(nullptr_t)` 供内部使用；CNI 仅暴露 `use_shell(program)`。

### 3.3 arguments() 约束

- 每个 `process_builder` 实例最多调用一次 `arguments()`。
- 重复调用抛 `mpp::runtime_error`。

---

## 4. mpp_impl 平台接口

平台特定实现，不直接暴露给脚本。由 `mpp::process` 内部调用。

### 4.1 process_startup

```cpp
struct process_startup {
    std::vector<std::string> _cmdline;           // 命令行
    std::optional<std::string> _shell_program;   // shell 程序
    std::unordered_map<std::string, std::string> _env;  // 环境变量
    bool _inherit_env = true;                    // 继承父进程环境
    std::string _cwd = ".";                      // 工作目录
    redirect_info _stdin, _stdout, _stderr;      // 重定向信息
    bool merge_outputs = false;                  // 合并输出
    bool inherit_stdin = false;                  // 继承 stdin
    bool inherit_stdout = false;                 // 继承 stdout
    bool inherit_stderr = false;                 // 继承 stderr
    bool shell_mode = false;                     // shell 模式
};
```

### 4.2 process_info

```cpp
struct process_info {
    fd_type _tid;      // 线程句柄（仅 Windows）
    fd_type _pid;      // 进程句柄（Windows）或 PID（Unix）
    fd_type _stdin;    // stdin 管道写端
    fd_type _stdout;   // stdout 管道读端
    fd_type _stderr;   // stderr 管道读端
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
| `wait_timeout_ms` | `(info, timeout_ms, exit_code&, poll_interval_ms) -> bool` | 带超时等待 |
| `get_pid` | `(info) -> int` | 获取进程 ID |

### 4.4 平台实现差异

| 特性 | Windows | Unix |
|------|---------|------|
| 进程创建 | `CreateProcess` + `STARTUPINFO` | `fork` + `execvpe` |
| 等待 | `WaitForSingleObject` | `waitid(P_PID)` |
| 非阻塞检查 | `WaitForSingleObject(0)` | `waitid(WNOHANG\|WNOWAIT)` |
| 超时等待 | `WaitForSingleObject(timeout)` | 轮询 `nanosleep` + `waitid` |
| 进程树终止 | `CreateToolhelp32Snapshot` 枚举子进程 | `kill(-pgid)` |
| 环境变量 | `GetEnvironmentStrings` + `CreateProcess` | `environ` + `fork` 前构建 |
| 命令行引号 | MSVCRT 规则（反斜杠-引号双写） | 无特殊处理 |
| fd 清理 | N/A（句柄继承控制） | `close_range()` (Linux 5.9+) / `/dev/fd` (macOS) / brute-force |

---

## 5. 辅助类型

### 5.1 fdostream / fdistream

```cpp
#include <mozart++/fdstream>
```

`std::streambuf` 实现，包装原生 fd。由 `mpp::file::in_stream()` / `out_stream()` 和 `mpp::process::in()` / `out()` / `err()` 内部使用。

### 5.2 mpp::string_ref

```cpp
#include <mozart++/string>
```

LLVM 风格非拥有字符串视图。内部用于 PATH 搜索和字符串处理。

### 5.3 Stream\<T\>

```cpp
#include <mozart++/stream>
```

Java 风格惰性流，支持 `filter`、`map`、`collect` 等操作。内部用于 fd 枚举等场景。

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
