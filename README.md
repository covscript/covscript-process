# covscript-process

CovScript 的进程扩展，基于 Mozart++ 封装跨平台的子进程创建、等待、管道、输出采集与异步文件 I/O 能力。

## Features

- `process.exec(command, args)` 直接启动子进程
- `new process.builder` 构造可复用的进程配置对象
- `wait()` / `try_wait()` / `wait_poll()` / `wait_with()` / `wait_for()` / `wait_until()` 等待进程结束
- `communicate()` 同时收集 stdout / stderr，避免单管道阻塞
- `get_pid()` / `kill()` 进程控制
- `process.async` 事件循环能力（`poll`/`poll_once`/`stop`/`restart`）
- `process.async.fstream()` 与 `file_t`：异步文件 I/O 与 `redirect_out`/`redirect_err` 组合使用

## API Notes

当前稳定的脚本侧构造方式：

```covscript
import process

var b = new process.builder
b.cmd("echo hello")
b.shell("cmd") # Linux/macOS 用 "/bin/sh"
var p = b.start()
system.out.println(p.wait())
```

注意：

- `process.builder` 是类型，不是工厂函数
- 应使用 `new process.builder` 创建实例
- `shell(string)`：传入 shell 程序路径（如 `cmd` 或 `/bin/sh`）启用 shell 模式
- `shell_off()`：关闭 shell 模式
- `arg()` / `arguments()` 每个 builder 只能调用一次，再次调用会抛异常

### 快捷 shell 启动

```covscript
import process

var b = new process.builder
b.cmd("echo shell_ok")
b.shell("cmd") # Linux/macOS 用 "/bin/sh"
var p = b.start()
var r = p.communicate()
system.out.println(r[0])
```

### 协程 / 框架兼容

等待 API 提供三档语义，按需选择：

| API | 阻塞行为 |
|---|---|
| `wait()` / `communicate()` | 真正的内核阻塞，会挂起当前 OS 线程，不让步给同线程的 fiber / 事件循环 |
| `wait_poll(timeout, interval)` | 在 CNI 层按 `interval` 轮询。若 SDK 支持 fiber 且当前正运行在 fiber 中，则自动调用 `cs::fiber::yield()` 让出执行权；否则 `sleep_for(interval)` |
| `wait_with(timeout, callback)` | 同样按超时窗口轮询，但每轮迭代调用 `callback()` 替代 yield/sleep。把宿主框架自己的调度原语（事件循环 tick、`runtime.delay`、自定义 fiber yield 等）传进来即可无侵入嵌入 |

```covscript
# 1) 默认即可：纯脚本场景。在 fiber 中会自动让步
var code = p.wait_poll(5000, 5)

# 2) 嵌入到外部异步框架：每次轮询时驱动一次框架的事件循环
function my_tick()
    # 例如：runtime.delay(1)；或调用框架的 schedule_yield()；或推进 reactor
end
var code2 = p.wait_with(5000, my_tick)
```

注意：`wait_with` 的回调内 **必须**自己负责让步或 sleep；空回调（如纯递增计数）会让 CNI 层退化成 100% CPU 忙等。

### `process.async` 与 `file_t`

```covscript
import process

var f = process.async.fstream("./out.txt", "w")
var b = new process.builder
b.cmd("echo redirected")
b.shell("cmd") # Linux/macOS 用 "/bin/sh"
b.redirect_out(f)
var p = b.start()
p.wait()
f.flush(1000)
f.close()
```

## Build

项目依赖 CovScript SDK，通过环境变量 `CS_DEV_PATH` 提供：

- Windows: 指向本地 CovScript SDK 根目录，例如 `D:/.../covscript/csdev`
- Linux: 指向已安装 SDK 根目录，例如 `/usr/share/covscript`

### Windows (MinGW)

```powershell
mkdir cmake-build\mingw-w64
cd cmake-build\mingw-w64
cmake ..\..
mingw32-make -j4
```

### Linux / WSL

```bash
mkdir -p cmake-build/linux
cd cmake-build/linux
CS_DEV_PATH=/usr/share/covscript cmake ../..
cmake --build . -- -j4
```

### 解释器基线

- 当前升级后的解释器基线：`COVSCRIPT_ABI_VERSION = 251109`
- 本扩展 fiber 协作分支门槛是 ABI `>= 250908`，因此在当前解释器上默认启用 fiber 兼容路径
- 构建标准来自 SDK 的 `csbuild.cmake`（当前为 C++17）

## Test

### Windows full test

```powershell
cs -i .\build\imports .\test_unit.csc
cs -i .\build\imports .\test_async.csc
cs -i .\build\imports .\test_file_redirect.csc
```

### Linux smoke tests

```bash
cs -i ./build/imports ./test_linux_smoke.csc
cs -i ./build/imports ./test_linux_exec.csc
cs -i ./build/imports ./test.csc
cs -i ./build/imports ./test_async.csc
cs -i ./build/imports ./test_file_redirect.csc
```

## Project Layout

- `process.cpp`：CovScript CNI 绑定层
- `src/process.cpp`：平台无关的进程创建胶水层
- `src/process_win32.cpp`：Windows 进程实现
- `src/process_unix.cpp`：Unix / Linux 进程实现
- `include/mozart++/mpp_system/process.hpp`：公共 API 与 builder / process 类型定义
- `test_unit.csc`：主回归测试（含 wait_with/wait_until）
- `test_async.csc`：事件循环与异步文件 I/O 回归
- `test_file_redirect.csc`：file_t 重定向回归
- `test_linux_smoke.csc` / `test_linux_exec.csc`：Linux 冒烟验证

## Known Constraints

- `builder.shell(...)` 内部会把 `cmd()` + `arg()` 拼接成一条 shell 命令串，shell 元字符会被重新解释；需要精确参数语义时，优先使用非 shell 模式（`shell_off()`）

- 兼容提示：请统一通过 `import process` 使用本扩展，避免与解释器内置 `process` 冲突。
- 非 shell 模式（直接 `cmd()` + `arg()`，且 `shell_off()`）下，Windows 按 MSVCRT 标准规则转义参数；Unix 端 fork+exec 直接接收 argv

## 完整 API

完整 API 说明、统一语义约束与阶段性设计决策见 [API.md](API.md)。