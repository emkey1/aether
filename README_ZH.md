# iSH-AOK

iSH-AOK 是 [ish-app/ish](https://github.com/ish-app/ish) 的一个分支（fork），在此基础上添加了用于日常开发的产品、工具链和平台相关改动。

Testflight: https://testflight.apple.com/join/X1flyiqE

这个分支不只是改个名字。它包含了分支专属的行为、内置的根文件系统、诊断相关工作、File Provider 集成，以及正在进行中的实验性 amd64/x86_64 客户机支持。如果你想要上游的 iSH，请使用 `ish-app/ish`。如果你正在这个仓库中进行开发，这份 README 才是你需要参考的文档。

## 本分支新增的内容

- 分支专属的应用标识:
  - 产品名 `iSH-AOK`
  - Bundle root `app.ish.iSH-AOK`
- 内置在应用中的根文件系统:
  - `i386` 的 `root.tar.gz`（`Devuan5(Debian12)`）
  - `alpine-minirootfs-3.23.3-x86.tar.gz`（`Alpine3.23.3`）
  - `alpine-minirootfs-3.23.3-x86_64.tar.gz`（`Alpine3.23.3(x86_64)`）
- 通过 iOS 系统 API 暴露客户机文件的 File Provider 支持。
- 该分支专属的额外诊断与运维相关改动。
- 正在进行中的 amd64 解释器、加载器及系统调用相关工作。

## 当前 amd64 状态

本仓库中的 amd64 相关工作仍处于实验阶段。

- 应用可以导入并尝试引导 `x86_64` 客户机根文件系统。
- 解释器、ELF64 加载器和 amd64 系统调用路径正在积极开发中。
- 目前可能出现启动早期失败、指令解码缺口以及用户态程序只能部分运行的情况。
- 该项工作目前对应的开发分支通常是 `amd64`。

相关文件:

- [amd64_port_plan.md](docs/amd64_port_plan.md)
- [emu/amd64_interp.c](emu/amd64_interp.c)
- [kernel/exec.c](kernel/exec.c)
- [kernel/calls.c](kernel/calls.c)

## 仓库结构

- `app/`: iOS 应用、界面、根文件系统选择、诊断、File Provider 集成。
- `emu/`: 客户机 CPU 仿真，包含 amd64 解释器相关工作。
- `kernel/`: 系统调用转换、进程模型、exec、信号、内存管理。
- `fs/`: 文件系统层与 fakefs 集成。
- `jit/`: 继承自 iSH 的线程化代码 JIT 机制。
- `tests/`: 手动及自动化测试辅助工具。
- `tools/`: 开发者工具及主机侧辅助脚本。

## 克隆

本仓库使用了子模块（submodule）。

```bash
git clone --recurse-submodules git@github.com:emkey1/ish-AOK.git
cd ish-AOK
```

如果你已经在没有子模块的情况下克隆过：

```bash
git submodule update --init --recursive
```

## 构建依赖

进行本地开发通常需要：

- Xcode
- Python 3
- Meson
- Ninja
- Clang/LLVM 工具链
- sqlite3
- libarchive

在 macOS 上，常见的安装方式是：

```bash
brew install meson ninja llvm libarchive
```

`sqlite3` 通常已经预先安装好了。

## 构建 iOS 应用

用 Xcode 打开 [iSH-AOK.xcodeproj](iSH-AOK.xcodeproj)，构建 `iSH` scheme。

分支专属的重要设置：

- Bundle ID 由 [app/iSH.xcconfig](app/iSH.xcconfig) 控制。
- `ROOT_BUNDLE_IDENTIFIER` 默认值为 `app.ish.iSH-AOK`。
- 项目已经使用了分支专属的调试配置 `Debug-ApplePleaseFixFB19282108`。

命令行构建：

```bash
xcodebuild \
  -project iSH-AOK.xcodeproj \
  -scheme iSH \
  -sdk iphonesimulator \
  -configuration Debug-ApplePleaseFixFB19282108 \
  build CODE_SIGNING_ALLOWED=NO
```

如果仓库根目录下存在以下归档文件，iOS 构建脚本会把它们复制进应用包：

- `root.tar.gz`
- `alpine-minirootfs-3.23.3-x86.tar.gz`
- `alpine-minirootfs-3.23.3-x86_64.tar.gz`

如果缺少某个文件，对应的内置根文件系统将无法使用。

## 发布自动化（渐进式）

本仓库现在包含一个简单的辅助脚本：

- [`tools/release-aok.sh`](tools/release-aok.sh)

先从安全的部分开始：

```bash
./tools/release-aok.sh preflight
./tools/release-aok.sh archive
./tools/release-aok.sh export latest /tmp/iSH-AOK-export
```

这样你就能在继续手动上传的同时，获得一个可重复执行的归档 + IPA 导出流程。

当你准备好进行完整的 TestFlight 自动化时，使用：

```bash
./tools/release-aok.sh upload-fastlane
```

`upload-fastlane` 使用现有的 `fastlane upload_build` lane，需要你配置好 Ruby/Bundler/Fastlane 以及签名/认证相关的密钥。

如果 `preflight` 提示 Ruby 版本过旧，运行：

```bash
brew install ruby@3.3
echo 'export PATH="/opt/homebrew/opt/ruby@3.3/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
bundle install
```

## 构建原生 CLI / 模拟器

对于模拟器（emulator）相关的开发，通常 Meson 构建比完整的 Xcode 构建更快。

初始设置：

```bash
meson setup build
```

增量构建：

```bash
ninja -C build
```

常用目标：

- `build/ish`
- `build/libish.a`

对于大多数模拟器相关的改动，这样就足够了：

```bash
ninja -C build libish.a
```

## 回归测试

本代码树中既有 `tests/e2e/` 下的端到端测试，也有 `tests/manual/` 下针对性的客户机侧探测程序。

最相关的原子操作/JIT 回归探测程序是：

- [tests/manual/atomics32.c](tests/manual/atomics32.c)
- [tests/manual/atomic_xadd32.c](tests/manual/atomic_xadd32.c)
- [tests/manual/atomic_cmpxchg32.c](tests/manual/atomic_cmpxchg32.c)
- [tests/manual/atomic_cmpxchg8b.c](tests/manual/atomic_cmpxchg8b.c)
- [tests/manual/atomic_logic32.c](tests/manual/atomic_logic32.c)
- [tests/manual/futex_core.c](tests/manual/futex_core.c)
- [tests/manual/signal_core.c](tests/manual/signal_core.c)
- [tests/manual/signal_restart.c](tests/manual/signal_restart.c)
- [tests/manual/signal_realtime.c](tests/manual/signal_realtime.c)
- [tests/manual/signal_altstack.c](tests/manual/signal_altstack.c)
- [tests/manual/signal_poll.c](tests/manual/signal_poll.c)
- [tests/manual/eventfd_interrupt.c](tests/manual/eventfd_interrupt.c)
- [tests/manual/amd64_regress.c](tests/manual/amd64_regress.c)
- [tests/manual/test_common.h](tests/manual/test_common.h)

`atomics32.c` 是统领性的探测程序。其余拆分出来的程序设计为在客户机内部编译，并且在结果不匹配时会以非零状态退出，因此可以作为以下方面的可重复回归测试目标：

- 带锁的 `xadd`
- 带锁的 `cmpxchg`
- 带锁的 `cmpxchg8b`
- 带锁的逻辑运算及相邻的标志位消费者
- futex 等待/唤醒、超时、信号中断及重启行为
- 信号投递、挂起掩码、`sigtimedwait`、`signalfd`、`sigsuspend` 以及面向线程的信号
- `poll`/`select`/`pselect`/`ppoll` 的信号中断及 EINTR 语义
- `SA_RESTART` 下阻塞型系统调用的重启行为
- 带负载的排队实时信号
- 备用栈（alternate-stack）信号处理程序的投递
- eventfd 读取中断以及通过共享等待路径实现的 `SA_RESTART` 行为
- 与 amd64 相关的回归，包括跨页 COW 写入、exec 加载器清零、`fcntl` 锁生命周期竞争，以及客户机 `gcc` 下 `cc1` 的压力测试

对于应用内置的根文件系统，或是在 iSH-AOK 下运行的导入根文件系统，客户机侧的设置辅助脚本是：

- [tests/manual/setup-regressions.sh](tests/manual/setup-regressions.sh)

在客户机内部，对应的 `/AOK/tests/setup-regressions.sh` 可以准备、构建并运行这套针对性测试。

所有针对性的客户机侧回归测试都接受 `-v` 或 `--verbose` 参数。不带该参数时，只会打印失败项以及最终的 `PASS`/`FAIL` 结果行。

## 处理根文件系统

本分支目前在应用中提供三种内置选项：

- `i386` 的 `Devuan5(Debian12)`
- `i386` 的 `Alpine3.23.3`
- `amd64` 的 `Alpine3.23.3(x86_64)`

根文件系统选择界面及元数据处理位于：

- [app/Roots.m](app/Roots.m)
- [app/RootsTableViewController.m](app/RootsTableViewController.m)

说明：

- `x86_64` 根文件系统是用于开发调试的，并非面向普通终端用户。
- 应用会为每个导入的根文件系统记录客户机 ABI。
- 对于受管理的根文件系统，File Provider 域会保持同步。

## 日志与诊断

日志由 [app/iSH.xcconfig](app/iSH.xcconfig) 中的 `ISH_LOG` 控制。

示例：

```xcconfig
ISH_LOG = verbose strace
```

当前日志记录器默认值：

- iPhone / 模拟器: `nslog`
- macOS: `dprintf`

常用的日志类型：

- `strace`
- `verbose`
- `instr`

对于模拟器相关的开发，通常最快的循环是：

1. 修改解释器或内核代码。
2. 运行 `ninja -C build libish.a`。
3. 重新构建 iOS 应用。
4. 在模拟器中启动。
5. 查看控制台日志，检查出错的 RIP、opcode 窗口以及寄存器状态。

## File Provider

本分支包含一个 iOS File Provider 扩展，用于通过系统文件 API 暴露客户机文件。

相关代码：

- [app/FileProvider/FileProviderExtension.m](app/FileProvider/FileProviderExtension.m)
- [app/FileProvider/FileProviderEnumerator.m](app/FileProvider/FileProviderEnumerator.m)
- [app/FileProvider/FileProviderItem.m](app/FileProvider/FileProviderItem.m)

这是本分支专属的功能，应视为此处所维护产品能力的一部分。

## amd64 开发流程

如果你正在这个仓库中进行 amd64 相关的开发：

- 优先关注解释器，不要假设 JIT 路径目前是相关的。
- 保持改动小而可回退。
- 用以下两种方式进行验证：
  - `ninja -C build libish.a`
  - 模拟器上的 `xcodebuild`
- 当客户机出现失败时，请记录：
  - 故障类型
  - 客户机 RIP
  - opcode 窗口
  - 客户机寄存器
  - 任何额外添加的针对性追踪输出

当前 amd64 相关工作经常涉及：

- [emu/amd64_interp.c](emu/amd64_interp.c) 中的指令解码与执行
- [jit/jit.c](jit/jit.c) 中的 JIT 交接与分发
- [kernel/exec.c](kernel/exec.c) 中的 ELF64 与进程启动
- [kernel/calls.c](kernel/calls.c) 中的系统调用分发与 ABI 处理

## 分支说明

截至撰写本文档时：

- `working` 是本分支的活跃集成分支。修复、功能开发和发布候选都会先合并到这里。
- `main` 跟踪已合并、稳定的代码，在发布上线时会从 `working` 更新。
- `amd64` 是 x86_64 客户机支持开发的活跃分支。
- `aarch64` 是原生 ARM64 客户机支持开发的活跃分支（参见 [aarch64_guest_plan.md](docs/aarch64_guest_plan.md)）。

如果你更新了跨分支的文档，请确保相关分支保持同步。

## 与上游的关系

iSH-AOK 基于上游 iSH，但已经有意地进行了分化。

这意味着：

- 上游 README 中的说明在本分支中可能不完整或不适用
- 分支名称与构建配置可能不同
- 内置根文件系统及运维行为是本分支专属的
- 不应假设本分支中的实验性 amd64 支持在上游也存在

## 致谢

`aarch64` 分支的原生 ARM64 客户机支持工作，受到 [OpenMinis/ish-arm64](https://github.com/OpenMinis/ish-arm64) 的启发，部分内容参考自该项目。它是 `ish-app/ish` 的一个 GPLv3 分支，独立实现了同样的能力。
详细的文件级致谢参见 [CREDITS-aarch64.md](docs/CREDITS-aarch64.md)。

## 许可证

参见：

- [LICENSE.md](LICENSE.md)
- [LICENSE.IOS](LICENSE.IOS)
