# PrimeShim Android

**中文** | [English](#english)

HP Prime 固件的 Android 兼容层。PrimeShim 不模拟 CPU，而是把原厂 ARM ELF 直接作为 32 位原生进程运行，并在运行时把 BESTA 系统调用和硬件访问接到
Android/Linux 实现上。

> 本项目不附带任何 HP 固件，请使用你合法拥有的固件。

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
- 支持 HP Prime 20260909 与 20250915 固件
- 320×240 LCD、触摸、多点触控、屏幕键盘
- 中文 / English / Français
- 亮色 / 暗色 / 跟随系统主题
- 可调字体大小
- 可拖动菜单按钮
- 日志、截图、A:/C: 文件管理、备份/恢复

### JavaScript 脚本

菜单 → **脚本** 打开内置编辑器。脚本可以直接控制固件按键和触摸：

```js
await prime.press("1");
await prime.press("+");
await prime.press("2");
await prime.press("enter");

await prime.touchTap(0, 160, 120);   // id, x, y
prime.log("done");
```

支持的 API：

- `prime.press(name)`：按下并松开一个键，`await` 等待 60ms
- `prime.key(name, down)`：原始按下/松开，`down` 为 `true`/`false`
- `prime.keyDown(name)` / `prime.keyUp(name)`
- `prime.touchDown(id, x, y)` / `prime.touchMove(...)` / `prime.touchUp(...)`
- `prime.touchTap(id, x, y)`
- `prime.sleep(ms)`：返回 Promise，配合 `await`
- `prime.log(...)`：写入脚本日志
- `prime.keys()`：返回可用键名 JSON

触摸坐标使用固件坐标 `x=0..319`、`y=0..239`。按键名例如
`enter`、`shift`、`alpha`、`sin`、`cos`、`tan`、`home`、`esc`，
也接受数字或十六进制键码。

“导入”可以读取外部 `.js` 文件；脚本保存在应用设置中，下次打开编辑器
会恢复。脚本在 WebView 中运行，不启用网络、文件或 DOM 访问。

### 设备要求

- Android 8.0+
- **必须支持 32 位 ARM ABI `armeabi-v7a`**，仅 64 位 ARM 的设备无法运行

### 安装

从 [Releases](https://github.com/SQY419/PrimeShim/releases) 下载最新 APK，然后安装：

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
“备份 - 备份 DATA”中导入 C:\DATA 的 zip。

### 构建

```sh
cd android
gradle :app:assembleDebug
```

或使用 Android Studio 打开 `android/`。

公开构建使用 Android 默认 debug 签名。

### 已知问题

- 未测试 20250915 以下的固件，不保证可用性。
- 不能执行后门程序。

### 致谢

- [Natsuki-Min/PrimeShim](https://github.com/Natsuki-Min/PrimeShim)：原始兼容层
- [telecomadm1145/PrimeU2](https://github.com/telecomadm1145/PrimeU2)：BESTA/HLE 参考实现
- Material Components for Android

本项目不包含 HP 的固件。这些内容由用户自行导入。

欢迎提交 issues 和 pull request。

---

<a id="english"></a>
## English

PrimeShim is an Android compatibility layer for HP Prime firmware. It does not
emulate the CPU: it runs the original ARM ELF natively on 32-bit ARM and patches
BESTA system calls and hardware accesses into Android/Linux implementations at
runtime.

> This repository does not ship HP firmware or A:/C: data. Use firmware you are
> legally allowed to use. This public snapshot intentionally contains no signing
> key and no proprietary HP binaries.

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

### JavaScript scripting

Open **Menu → Script** to use the built-in editor. Scripts can control
firmware keys and touch input directly:

```js
await prime.press("1");
await prime.press("+");
await prime.press("2");
await prime.press("enter");

await prime.touchTap(0, 160, 120);   // id, x, y
prime.log("done");
```

API:

- `prime.press(name)`: press and release a key; await the 60 ms gap
- `prime.key(name, down)`: raw press/release
- `prime.keyDown(name)` / `prime.keyUp(name)`
- `prime.touchDown(id, x, y)` / `prime.touchMove(...)` / `prime.touchUp(...)`
- `prime.touchTap(id, x, y)`
- `prime.sleep(ms)`: returns a Promise for `await`
- `prime.log(...)`: append to the script log
- `prime.keys()`: return the available key names as JSON

Touch coordinates use firmware space: `x=0..319`, `y=0..239`. Key names include
`enter`, `shift`, `alpha`, `sin`, `cos`, `tan`, `home`, `esc`; numeric and hex
key codes are also accepted.

**Import** loads an external `.js` file. The editor content is stored in app
settings and restored next time. Scripts run in a WebView with network, file and
DOM access disabled.

### Requirements

- Android 8.0 or newer
- **A 32-bit ARM ABI (`armeabi-v7a`) must be available**
- 64-bit-only ARM devices are not supported

### Install

Download the latest APK from [Releases](https://github.com/SQY419/PrimeShim/releases)
and install it:

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
