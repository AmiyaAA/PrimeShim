# PrimeShim Android

**中文** | [English](#english)

HP Prime 固件的 Android 兼容层。PrimeShim 不模拟 CPU，而是把原厂 ARM ELF
直接作为 32 位原生进程运行，并在运行时把 BESTA 系统调用和硬件访问接到
Android/Linux 实现上。

> 本项目不附带任何 HP 固件或 A:/C: 数据。请使用你合法拥有的固件。

---

## 中文

### 功能

- 直接运行为 HP Prime 编译的 `armfir.elf`
- 从官方升级包导入固件：
  - 支持整个官方 zip
  - 支持 `PRIME_APP.DAT`
  - 支持一次多选 `PRIME_APP.DAT`、`PRIME_MASTER.DAT`、`PRIME_OS.ROM`
- 自动从 `PRIME_APP.DAT[0x2000:]` 解析 FAT16 A: 盘，保留 VFAT 长文件名大小写
- 自动生成 `A:\dummy.DAT` 字体/帮助数据库
- 每个固件 ELF 使用独立 profile，A: 盘始终随固件重建，C: 用户数据保留
- 支持 HP Prime 2.4.2 与 0915 固件
- 320×240 LCD、触摸、多点触控、屏幕键盘
- 中文 / English / Français
- 亮色 / 暗色 / 跟随系统主题
- 可调字体大小
- 可拖动菜单按钮
- 日志、截图、A:/C: 文件管理、备份/恢复

### 设备要求

- Android 8.0 或更高
- **必须支持 32 位 ARM ABI `armeabi-v7a`**
- 仅 64 位 ARM 的设备无法运行

### 安装

从 GitHub Actions 下载 `primeshim-debug-apk`，然后安装：

```sh
adb install -r app-debug.apk
```

### 首次使用

1. 打开 PrimeShim
2. 点击“导入官方固件”
3. 选择 HP 官方升级包 zip，或包里的 `PRIME_APP.DAT`
4. 等待 A: 盘解包和 `APPSLIST.MD5` 校验
5. 启动

`PRIME_MASTER.DAT` 和 `PRIME_OS.ROM` 不需要导入，应用会识别并忽略。

官方升级包不包含用户 C: 盘。需要恢复用户程序和数据时，请在
“备份 → 备份 DATA”中导入 C:\DATA 的 zip。

### 构建

```sh
cd android
gradle :app:assembleDebug
```

或使用 Android Studio 打开 `android/`。

仓库不包含签名私钥；公开构建使用 Android 默认 debug 签名。

### 操作

- 点按计算器屏幕：单点触摸
- 双指：固件支持的多点手势
- 长按左上角菜单按钮：拖动位置
- 点按菜单按钮：打开菜单
- 设置标题右侧的太阳/月亮图标：切换亮暗主题
- 设置 → 语言：中文 / English / Français
- 设置 → 外观：主题和字体大小

### 致谢

- [Natsuki-Min/PrimeShim](https://github.com/Natsuki-Min/PrimeShim)：原始兼容层
- [telecomadm1145/PrimeU2](https://github.com/telecomadm1145/PrimeU2)：BESTA/HLE 参考实现
- Material Components for Android

本项目不包含 HP 的固件、字体数据库或 A:/C: 数据；这些内容由用户自行导入。

---

<a id="english"></a>
## English

PrimeShim is an Android compatibility layer for HP Prime firmware. It does not
emulate the CPU: it runs the original ARM ELF natively on 32-bit ARM and patches
BESTA system calls and hardware accesses into Android/Linux implementations at
runtime.

> This repository does not ship HP firmware or A:/C: data. Use firmware you are
> legally allowed to use.

### Features

- Runs an unmodified HP Prime `armfir.elf`
- Imports official update packages:
  - the complete official zip
  - a bare `PRIME_APP.DAT`
  - multi-select of `PRIME_APP.DAT`, `PRIME_MASTER.DAT`, `PRIME_OS.ROM`
- Extracts the FAT16 A: volume from `PRIME_APP.DAT[0x2000:]`, preserving VFAT
  long-name case
- Creates `A:\dummy.DAT`, the generation-matched font/help database
- One profile per firmware ELF; A: is rebuilt from the selected package while
  C: user data is preserved
- Supports HP Prime 2.4.2 and 0915 firmware
- 320×240 LCD, touch, multi-touch and on-screen keyboard
- Chinese / English / French UI
- Light / dark / follow-system theme
- Adjustable font scale
- Draggable menu button
- Logs, screenshots, A:/C: file manager and backup/restore

### Requirements

- Android 8.0 or newer
- **A 32-bit ARM ABI (`armeabi-v7a`) must be available**
- 64-bit-only ARM devices are not supported

### Install

Download `primeshim-debug-apk` from GitHub Actions and install it:

```sh
adb install -r app-debug.apk
```

### First run

1. Start PrimeShim.
2. Tap **Import official firmware**.
3. Pick the official HP update zip or `PRIME_APP.DAT` from it.
4. Wait for A: extraction and `APPSLIST.MD5` verification.
5. Start the calculator.

`PRIME_MASTER.DAT` and `PRIME_OS.ROM` are recognised and ignored; they are not
needed by PrimeShim.

The official package does not contain the user C: drive. To restore user
programs and data, use **Backup → Back up DATA** with a `C:\DATA` zip.

### Build

```sh
cd android
gradle :app:assembleDebug
```

Or open `android/` in Android Studio.

The repository does not include the private signing key; public builds use the
Android default debug signature.

### Controls

- Tap the calculator surface: single touch
- Two fingers: firmware multi-touch gestures
- Long-press the top-left menu button: drag it
- Tap the menu button: open the menu
- Sun/moon icon beside the Settings title: toggle light/dark
- Settings → Language: Chinese / English / Français
- Settings → Appearance: theme and font size

### Credits

- [Natsuki-Min/PrimeShim](https://github.com/Natsuki-Min/PrimeShim): original
  compatibility layer
- [telecomadm1145/PrimeU2](https://github.com/telecomadm1145/PrimeU2): BESTA/HLE
  reference
- Material Components for Android

No HP firmware, font database or A:/C: data is included in this repository;
users must import their own.
