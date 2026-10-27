# PrimeShim 分析报告

> 仓库：<https://github.com/Natsuki-Min/PrimeShim>（`main`，本地：`/workspace/primeshim`）
> 分析日期：2026-10-23 · 分析方式：实际解包、源码通读、**成功编译并在 QEMU 中运行固件**

---

## 1. 一句话结论

**这是一个 Wine 式的兼容层，让 HP Prime 计算器的原厂固件（ARM ELF）直接以 Linux ARM 进程的身份跑起来**——不是 CPU 模拟器，而是把固件里 742 个 `SDKLIB_*` 软中断（SWI）和硬件寄存器访问，在**首次执行触发故障的那一刻**就地改写成跳进 Linux 侧 C 实现的跳板。

作者自己已宣布停止开发，理由写在 README 第一行：*ARM9 的小 TLB + HP 固件糟糕的代码局部性，在 Linux 4 KiB 页映射下只有原 RTOS 大约一半的性能*。

---

## 2. 仓库元数据（GitHub API 实测）

| 项 | 值 |
|---|---|
| 描述 | 无（description 为空） |
| 主语言 | Assembly 30.4 MB / C 351 KB / Python 10 KB（Assembly 占比高是因为 `text.asm` 30 MB 反汇编） |
| 创建 / 最后 push | 2025-08-30 / 2026-08-18 |
| 提交数 | **9 个**（`firstcommit` → … → `Document project status`） |
| Star / Fork | 3 / 0 |
| LICENSE | **无**（重要，见 §8） |
| 本地体积 | 56 MB（其中固件 46 MB、虚拟磁盘 8.8 MB） |

提交信息也很有信息量：
- `2025-08-30 firstcommit`（最初只有 README）
- `2026-02-18 add real machine support for Prime Linux`（真机支持）
- `2026-08-03 first init` → `2026-08-16 modify to a advanced framework and can work now`（重构为现在的驱动总线框架，"can work now"）
- `2026-08-18 Document project status`（停更声明）

---

## 3. 目录与代码量

```
primeshim/
├── Makefile                  4.3 KB，两套 profile（pc / device）
├── docs/PROJECT.md           158 行，真正的开发手册（比 README 更硬核）
├── include/                  8 个头文件：platform / runtime_patcher / peripheral / device / ui / host_ipc / log / usbdhid
├── src/                      368 KB，14 个 C + 1 个汇编
│   ├── main.c                211   启动编排
│   ├── runtime_patcher.c    1325   ELF 加载 + 故障打补丁 + 跳板生成（核心）
│   ├── system_shim.c        1063   线程/事件/设备/内存/电源
│   ├── filesystem.c         1654   DOS 风格 VFS（A: / C: 盘）
│   ├── keyboard.c            673   矩阵键盘 + 注入
│   ├── mouse.c               270   多点触控
│   ├── timer.c               290   硬件定时器陷阱 + 反编译函数复刻
│   ├── usbdhid.c             518   ConfigFS HID gadget（仅 device）
│   ├── platform/lcd_device.c 765  DRM + fbdev
│   ├── platform/lcd_pc.c     223  memfd 共享帧缓冲 + 管道输入
│   ├── host/sdl_host.c       375  宿主 SDL2 窗口 + 拉起 QEMU 子进程
│   ├── runtime_dispatch.S     27   外围回调桥
│   ├── runtime_syscalls.c     40   由脚本生成的注册表
│   └── syscall_stubs.c       747   **742 个 weak 存根**
├── tools/
│   ├── interruptmap.txt      742 条 SWI → 名字映射
│   ├── generate_shim.py      232 扫描固件字节模式生成汇编/映射
│   ├── generate_syscall_registry.py  41
│   ├── arm_pc_sampler.c / arm_process_watch.c  真机性能分析
│   └── myusb_gadget, gdb/segf.gdb, hid_test.py
├── firmware/2025-15270/      固件 46 MB（armfir.elf 8.2 MB + dat 8.9 MB + text.asm 30 MB）
├── runtime/prime_data/       虚拟 A:/C: 盘（HP 应用、列表、矩阵、设置）
├── third_party/              ARM 版 libdrm（e_machine=40，ARM）
└── tests/device_test.c       78 行原生单元测试
```

自写代码 **10,005 行**（src 8,691 + include 522 + tools 714 + tests 78）。

---

## 4. 工作原理：三层机制

### 4.1 第一层：把固件原封不动映射进来

`runtime_patcher.c` 做的事：

1. 校验 `armfir.elf`（实测：`ELF32 / EM_ARM / ET_EXEC / entry 0x30604ae8 / 单个 PT_LOAD`）；
2. 在固件旁边**原子创建** `armfir.elf.map`（`mkstemp` + `fsync` + `rename`），实测生成 7,753,728 字节；
3. `mmap(MAP_PRIVATE)` 到 ELF 要求的虚拟地址——`0x30600000` 起，`0x75e08c` 字节代码/数据 + `0x606c` 零初始化段，共约 7.4 MB；
4. 干净页保持 file-backed（可在小内存机器上回收），BSS / 运行期写入 / 被打补丁的代码页按需 copy-on-write；
5. 固件目录只读时退回匿名内存拷贝。

映射段名是 `ER_RO` / `ER_RW` / `ER_ZI` —— 典型的 ARM RVCT/Keil scatter-file 风格，说明这是 ARM 工具链产物。

### 4.2 第二层：**故障时才打补丁**（最核心的设计）

传统做法是启动时扫全镜像找 SWI 指令，太慢且会改写代码。PrimeShim 反过来：**先注册，后按需触发**。

1. `Syscall User Dispatch`（`prctl(PR_SET_SYSCALL_USER_DISPATCH, INCLUSIVE_ON, ...)`，仅覆盖固件地址区间）让固件里的 SVC 触发 `SIGSYS`；
2. 内核不支持 SUD 时退到 **seccomp BPF**（`SECCOMP_RET_TRAP`）—— ARM 上 `SYS_USER_DISPATCH` 的等价物；
3. 再不行退到 **QEMU 的 `SIGILL`** 兜底（QEMU 用户态对未定义 SVC 的处理）；
4. 捕获信号后读 `si_addr`/上下文，查表得到 `SDKLIB_XXX`，在**固定跳板池**里生成 `ldr pc, [pc, #-4]` + 处理器地址（`0xe51ff004`），再在原指令处写回一条 `B` 跳板；
5. 跳板池固定 262,144 字节，**紧贴镜像之后分配**（实测 `0x30d65000`），保证在 ARM `B` 的 ±32 MiB 范围内；
6. 外围寄存器同理：注册 `0x51000000`（定时器）、`0x4c800000`（LCD）、`0x57000000`（RTC）、`0x0`（零页哨兵）等区间，只有真正访问到已注册地址并 fault 的 `ldr/str` 才被专门化；地址不匹配就恢复寄存器执行原指令。

这一步的巧妙之处：**固件字节在磁盘上永远不被修改**，补丁只存在于私有脏页里；换固件不需要重新生成任何汇编、不需要重新链接（`make run-pc FIRMWARE=/path/to/armfir.elf`）。

### 4.3 第三层：weak/strong 符号决定 syscall 实现

```c
// syscall_stubs.c —— 742 个全是 weak
__attribute__((weak)) uint32_t SDKLIB_OSCreateThread (void) {STUB_DEBUG();return 0;}
```

真实的 C 实现写在 `system_shim.c` / `filesystem.c` / `timer.c` 等文件里，是**同名强符号**。链接时强符号自动覆盖 weak 存根，注册表直接取函数地址即可。

- 存根保留的意义：**覆盖率即文档**（`grep -c weak` 就是总数），并且作者明确要求"不要删掉 weak 存根"。
- 实测覆盖率：**742 个中 74 个有强实现 ≈ 10%**，剩下 90% 走存根（返回 0 + debug 日志）。

`tools/generate_syscall_registry.py` 把 `interruptmap.txt` 编译成 `.inc` 注册表，**不修改固件**；`B` 还是 `BL`、要不要 `add sp, sp, #8` 拆 BESTA 栈帧，由 `RUNTIME_SYSCALL_BESTA_FRAME` 标记逐条决定。

---

## 5. 实测：我把它编出来并跑起来了

环境：ARM64 Ubuntu 24.04 proot，`gcc-arm-linux-gnueabi 13.3.0`、GNU Make 4.3、`qemu-arm-static`、SDL2 2.30。

### 5.1 编译（全部通过，零错误）

| 目标 | 结果 |
|---|---|
| `make HOST=device` | ✅ `build/device/primeshim.elf` **286,596 B**（ARM ELF，链接 libdrm/socket/usbdhid） |
| `make HOST=pc` | ✅ `build/pc/primeshim.elf` 266,124 B + `build/pc/primeshim-sdl` 73,832 B |
| `make test` | ✅ 原生单元测试通过（驱动注册表、ioctl 分发、日志参数惰性求值） |

只有几条 `-Wunused-result`、`implicit declaration of memfd_create` 之类的无害警告；`runtime_dispatch.o` 缺 `.note.GNU-stack` 会被新版 ld 警告"可执行栈"。

### 5.2 加载验证（`--patch-only`）

```
INFO [runtime] loaded firmware/2025-15270/armfir.elf at 0x30600000-0x30d65000,
     entry 0x30604ae8 (file-backed)
INFO [runtime] armed fault-time patching; 262144-byte trampoline pool at 0x30d65000 (no firmware scan)
```
并在固件旁生成了 `armfir.elf.map`（7.75 MB）——完全符合 README 描述。

### 5.3 完整启动（QEMU + SDL dummy）

```
INFO  [peripheral] started pc-sdl-ipc (classes 0x7)
DEBUG [firmware ] check sdk version
DEBUG [firmware ] SDKLIB: A:\WINDOW\SYSTEM\SDKLIB.DLL
DEBUG [firmware ] check data format, read header...
INFO  [timer    ] hardware timer initialized (period 13040000 ns)   ← 13.04 ms tick
DEBUG [thread   ] tid=28790 started creator=28786
DEBUG [device   ] unimplemented INDICATOR ioctl=0x101 in=4 out=0    ← 每秒一次
```

**固件真的跑起来了**：过了 SDK 版本检查、加载了虚拟 A 盘的 `SDKLIB.DLL`、读了数据区头部、起了工作线程、硬件定时器按 13.04 ms 周期跑、每秒轮询一次指示灯设备。整个 30 秒观察窗口内进程持续运行（`timeout` 才把它杀掉）。

### 5.4 帧缓冲抓取（真·截图）

通过 `PRIMESHIM_FRAMEBUFFER_FD=3` 把共享帧缓冲换成一个普通文件，拿到 320×240×4 的原始像素：

- 225,853 / 307,200 字节非零，**538 种不同像素值**
- 顶部约 13 行是**深蓝色标题栏**，中间大片白，底部一行是 **6 个分隔的菜单格**

→ 这正是 HP Prime 的主界面布局（顶部标题 + 白色工作区 + 底部软键菜单）。渲染管线（LCD 陷阱 → 共享内存 → 320×240 ARGB）是通的。

已保存为 `screenshots/fb.png`。

### 5.5 日志里只出现了 5 个"未实现"

`SDKLIB__FindResourceW`、`SDKLIB__FreeLibrary`、`SDKLIB_SetAutoPowerOff`、`SDKLIB_GetEventType`、`SDKLIB_SetEventType`，加上 `SetSystemVariable id=85/87` 和 `INDICATOR ioctl=0x101`。
说明 74 个强实现**恰好覆盖了启动到主界面这条关键路径**——这是"能跑"和"有 742 个 API"之间最真实的差距。

---

## 6. 其它值得记的设计点

| 机制 | 做法 |
|---|---|
| **两进程 PC 架构** | 原生 SDL2 进程开窗，`fork/exec` 拉起 QEMU 跑 ARM ELF，两者通过 `memfd` 共享像素、通过 `pipe` 传 16 字节定长输入事件（`PRIMESHIM_HOST_EVENT_*`）。这样 ARM sysroot 里**不需要 SDL2** |
| **设备驱动总线** | `primeshim_peripheral_driver_t` 声明独占类别（display / keyboard / pointer / USB）+ probe/start/stop；总线拒绝重复认领同一类，缺必需后端就启动失败（实测报 `failed to start host peripherals: No such device`） |
| **客户机设备注册表** | `CreateFile("\\\\?\\USB")` → 按大小写不敏感名字查驱动 → 句柄带每开上下文；`DeviceIoControl` 里**没有按名字分支**。内置 USB / BAT / ARCH / USBHOST / BACK / FACTORY / INDICATOR |
| **文件系统** | `A:\...` → `runtime/prime_data/A/...`，`C:\...` → `.../C/...`；实现的是 Win32 风格的 `_OpenFile/_afindfirst/_GetPrivateProfileString/...`（BESTA 是 DOS/WinCE 血统的 RTOS） |
| **线程模型** | BESTA 线程 → pthread；事件/临界区/消息队列/信号量全部映射到 Linux 原语 |
| **反编译代码复刻** | `timer.c` 里有 `FUN_3078875c` / `FUN_30ae8b88` / `FUN_30b476b4` 三个照反汇编逐行抄回来的固件函数，还硬编码了固件全局 `*(uint32_t*)0x30d375b0`（落在 ER_RO 段内）——文档里专门提醒"这类硬编码地址要谨慎" |
| **零页哨兵** | 固件用地址 0 的 4 个字节做全零判断；QEMU 用 `-B 0x100000000` 偏移客户机地址空间，真机则由 `main.c` 注册 4 字节 RAM 外设顶上 |
| **调试/测试** | `PRIMESHIM_TEST_KEY_SEQUENCE` 按键注入（走和物理键盘同一事件队列，且**阻塞等待队列空间**以免丢事件）；`make arm-pc-sampler` / `arm-process-watch` 做真机性能采样；`--continue-on-segfault` 跳过故障指令 |

---

## 7. 目标硬件的真实约束（README/PROJECT.md 明说的 ABI 要求）

- **ARMv5TE、soft-float EABI、32 位指针**（`-march=armv5te -mabi=aapcs-linux -mfloat-abi=soft`）
- 固定虚拟地址：固件 `0x30600000`、定时器 `0x51000000`、LCD `0x4c800000`、RTC `0x57000000`
- 只支持一个固件版本：**HP Prime OS 15270（2025-01-31）**；2013 年镜像与 PC/设备双份源码树都已被删
- 固件运行在 `0x30d65000` 之后的跳板池要落在 ±32 MiB 内（ARM B 指令范围）
- 映射**故意是 RWX**（既要打补丁又要执行）

---

## 8. 风险与限制（必读）

1. **没有 LICENSE 文件**。仓库里直接放着 HP 原厂固件 `armfir.elf`（8.2 MB）和反汇编 `text.asm`（30 MB）——这是 HP 的专有二进制，**公开分发几乎肯定侵权**。代码本身也无许可证 = 默认"保留所有权利"。想二次利用必须先解决授权。
2. **只对 15270 版本有效**。换固件的 ABI 仍需人工审计入口、映射、syscall 签名。
3. **依赖较新内核**：SUD 需要 Linux 5.11+ 且架构支持；否则退 seccomp（要 `PR_SET_NO_NEW_PRIVS`）；再否则只能靠 QEMU 的 SIGILL 兜底（性能最差）。
4. **90% 的 syscall 是空实现**。返回 0 的存根意味着未覆盖功能会静默"成功"——这是最难查的一类 bug。
5. **作者已停更**，明确说性能只有原 RTOS 的一半（ARM9 VIVT cache + 小 TLB 遇上 4 KiB 页）。仓库没有 issue 维护承诺。
6. 设备 profile 会真的动 `/sys/power/state`、背光、USB gadget、DRM——只能在目标计算器上跑。

---

## 9. 建议的阅读路线

```
1. README.md（10 分钟，行为与命令行）
2. docs/PROJECT.md（15 分钟，比 README 更接近实现的真相）
3. include/runtime_patcher.h + src/runtime_patcher.c:645-820（跳板生成）
4. src/runtime_patcher.c:1000-1230（SIGSYS/SIGILL 处理 + SUD/seccomp 注册）
5. src/syscall_stubs.c（742 个 API 的全景清单，就是 BESTA RTOS 的 API 目录）
6. src/system_shim.c（74 个强实现里的主力：线程/事件/内存/电源）
7. src/filesystem.c（DOS 风格 VFS，1654 行最能体现"兼容层"的脏活）
8. tools/interruptmap.txt + generate_shim.py（SWI 编号从哪来、外设访问模式怎么识别）
```

想动手的话：随便挑一个 `interruptmap.txt` 里的未实现项 → 在 `src/` 里加同名强函数 → `make check` → 跑 `PRIMESHIM_LOG_LEVEL=debug` 看有没有被调用。

---

## 10. 与相关项目的关系

README 自己写明了血统：**[Project Muteki](https://github.com/Project-Muteki/muteki)**（HP Prime 固件逆向/mod 生态）与 **[PrimeU2](https://github.com/telecomadm1145/PrimeU2)**。
- Muteki 系：更偏"在真机上跑/改固件"。
- PrimeU2：把 Prime 固件搬到其他硬件。
- **PrimeShim 的独特定位**：不在硬件上冒充，而是把固件当成一个**普通 Linux 用户态进程**来跑，所以能在 PC 上开发、能用 gdb、能用 SDL 窗口、能一键换固件。

---

## 附：本次分析产生的文件

| 文件 | 说明 |
|---|---|
| `/workspace/primeshim/` | 仓库内容（121 个文件，56 MB） |
| `/workspace/primeshim/build/device/primeshim.elf` | 我编译出的设备版（286 KB） |
| `/workspace/primeshim/build/pc/primeshim.elf` + `primeshim-sdl` | PC 版（266 KB + 74 KB） |
| `/workspace/primeshim/firmware/2025-15270/armfir.elf.map` | 运行后生成的 7.75 MB 预映射 |
| `/workspace/primeshim/screenshots/fb.png` | 从固件帧缓冲抓到的 320×240 主界面 |
| `/tmp/run.log` | 30 秒运行日志（54 行） |

*本报告中的编译结果、运行日志、帧缓冲统计均为本次实际执行所得，非推测。*
