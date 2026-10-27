# Android port

PrimeShim runs the HP Prime firmware as a Linux ARM process.  This document
describes the Android front-end added in `android/`, what is verified, and what
still has to be proven on real hardware.

## Design

```text
┌──────────────────────────── Android app process (32-bit, armeabi-v7a) ───┐
│  MainActivity.java (SurfaceView)                                        │
│      │  JNI: nativeStart / nativeSetSurface / nativeTouch / nativeKey    │
│      ▼                                                                  │
│  libprimeshim.so                                                        │
│    ├── primeshim_main()  ── runtime_patcher (same code as PC/device)     │
│    ├── lcd_android.c     ── 320×240 RAM framebuffer + compose thread     │
│    └── system_shim / filesystem / keyboard / timer / device registry     │
└─────────────────────────────────────────────────────────────────────────┘
                            │ ANativeWindow
                            ▼  Surface → compositor → screen
```

Decisions that differ from the desktop profile:

| Topic | Desktop (`HOST=pc`) | Android (`PRIMESHIM_HOST_ANDROID`) |
| --- | --- | --- |
| CPU | ARM ELF under `qemu-arm` | **native** ARMv7 execution, no emulator |
| Process model | two processes (SDL host + guest) | **one** process; Java is only a UI shell |
| Framebuffer | `memfd` shared with SDL window | plain RAM buffer in the library's BSS |
| Present | SDL2 texture | `ANativeWindow_lock` + software scale, 30 Hz |
| Input | pipe of 16-byte events | direct JNI calls |
| Power / USB | ignored | ignored (same as desktop) |

Keeping the framebuffer in the library's own `.bss` is deliberate: Android
10+ forbids `PROT_EXEC` mappings backed by app-writable files, and a shared
memfd would only add failure modes without buying anything.

## What is implemented

* `include/platform.h` — third profile `PRIMESHIM_HOST_ANDROID`, still exactly
  one profile per build.
* `src/platform/lcd_android.c` — LCD_MAGIC contract, aspect-preserving
  compose thread, touch/key injection, peripheral driver registration
  (display + keyboard + pointer, required).
* `android/jni/primeshim_jni.c` — lifecycle, surface hand-off, Android→Prime
  keycode table (mirrors the evdev table in `src/keyboard.c`).
* `android/` Gradle project — `armeabi-v7a` only, CMake builds the whole
  runtime as `libprimeshim.so`, Java imports the user's official update
  package and builds a per-firmware profile.
* `src/system_shim.c` — power-off is now ignored on *every* host except the
  calculator (previously only the PC branch was guarded).

## Verified / not verified

Verified in this workspace (ARM64 Linux, no Android device):

| Check | Result |
| --- | --- |
| `tools/check_android_compile.sh` | `lcd_android.c` and `primeshim_jni.c` compile clean against stub headers |
| Full core compile with `PRIMESHIM_HOST_ANDROID=1` | 14 translation units compile; only pre-existing upstream warnings |
| Link as `libprimeshim.so` (glibc cross toolchain, `-fPIC`) | succeeds, 345 KB, `Machine: ARM`, only libc/ld undefined |
| Static SVC patch on the real image | 742 sites rewritten, 0 failed, identical boot |
| Desktop/device regression (`make HOST=device`) | still builds |

Not verified — needs a real device with a 32-bit ABI and a kernel that allows
the patching mechanisms:

1. **Syscall interception — mitigated by default.** Android builds enable
   `PRIMESHIM_STATIC_SYSCALLS_DEFAULT=1`, so every registered SVC is rewritten
   at load time and no kernel feature is required.  The fault-driven path
   (SUD -> seccomp -> SIGILL) is still compiled in for anything the scan skips.
2. **Fixed-address mappings.** The firmware wants `0x30600000`, `0x4c800000`
   and `0x57000000`; the patcher maps them `MAP_FIXED`.  Bionic's library and
   heap layout must be checked for collisions on each device family.
3. **`PROT_EXEC` on patched pages.** Firmware pages are made executable at
   runtime; Android 10+ enforces W^X for file-backed mappings and may also
   restrict anonymous exec memory depending on target SDK and SELinux domain.
4. **Soft-float ABI.** The firmware is ARMv5TE **soft-float**; bionic is
   softfp/NEON.  Integer and pointer calls match, but any firmware callback
   taking/returning `float`/`double` would be miscompiled.  The known call
   sites are syscalls (integer only), so this is expected to be safe but is
   untested.
5. **Performance.** The author measured roughly half of RTOS speed on the real
   ARM9; running the same firmware inside an app process on a faster SoC is
   expected to be comfortably faster, but has not been measured.

## Static SVC patching (default on Android)

`runtime_patcher_static_patch_syscalls()` walks the executable ranges and emits
the same trampoline the fault handler would have emitted, for every `svc` whose
24-bit immediate is a registered syscall.  No signal, no seccomp, no SUD.

Measured on the 15270 image:

```text
static SVC patch: 742 sites rewritten, 93 unregistered immediates,
                  18626 conditional left to the fault path, 0 failed
```

* 742 = registered syscalls that actually occur in `ER_RO` (independently
  confirmed by scanning the ELF with Python).
* 93 = `svc` sites whose immediate is absent from `interruptmap.txt`.
* 18626 = words matching the SVC bit pattern with a conditional suffix.  Every
  float literal (`0x3f800000`...) looks like an SVC to a naive mask test, so the
  scan requires `cond == AL`; that is what protects literal pools.  Conditional
  SVCs stay on the fault path, where their condition code is preserved.
* Booting with `--static-syscall-patch` gives an **identical milestone
  sequence** to the fault-driven run (SDKLIB.DLL, data header, 13.04 ms timer,
  `launching firmware function 0x306a2cfc`, INDICATOR polling).

Cost: 742 x 128-byte trampolines = 95 KB of the 256 KB pool, plus one dirty page
per patched code page.

```sh
make run-pc RUN_ARGS=--static-syscall-patch
PRIMESHIM_STATIC_SYSCALLS=1 ./primeshim.elf --firmware armfir.elf
```

Disable with `--no-static-syscall-patch` or `PRIMESHIM_STATIC_SYSCALLS=0`.

## Signing and versioning

* **Stable signature.**  `android/keystore/primeshim.p12` (PKCS#12, alias
  `primeshim`, RSA 2048, valid until 2056) plus `android/keystore.properties`
  are committed to the private repository, so every CI run signs with the same
  certificate and upgrades install over the previous build.  Certificate
  SHA-256: `81:A7:FF:75:C4:6B:DF:14:D4:12:FC:C1:26:03:0B:89:FF:58:E6:E7:E9:CA:C4:05:6B:90:3F:FD:28:34:3C:02`.
  If the repository ever becomes public, treat the key as compromised: generate
  a new one and accept that users must uninstall once.
* **Version from the clock.**  `versionCode` is the UTC build date packed as
  `yyMMddHHmm` (e.g. `2610231313`) and `versionName` renders the same value as
  `20YY.MM.DD.HHMM` (`2026.10.23.1313`).  Builds therefore sort by time, and
  `PRIMESHIM_BUILD_STAMP` can override the stamp for reproducible builds.
  The stamp is also exposed as `BuildConfig.BUILD_STAMP` and shown on the
  "firmware missing" screen.

## Build

Prerequisites: Android Studio (AGP 8.5 / NDK 27), or `gradlew` + SDK + NDK
command line tools.

```sh
# 1. check the UI assets (no firmware/data is copied into the APK)
tools/prepare_android_assets.sh

# 2. build
cd android
./gradlew :app:assembleDebug        # or open this folder in Android Studio

# 3. install
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

The repository does not ship `gradle-wrapper.jar` (a binary blob).  Android
Studio generates the wrapper the first time the project is opened; for a pure
command-line build either run `gradle wrapper` once with Gradle 8.7+, or invoke
`gradle :app:assembleDebug` directly.  CI does the latter
(`.github/workflows/android-apk.yml`, which also accepts an optional
`FIRMWARE_B64` secret and uploads the APK as an artifact).

`local.properties` must point at your SDK (`sdk.dir=/path/to/Android/sdk`);
Gradle creates it automatically when opened in Android Studio.

### Device requirements

* 32-bit ARM ABI present (`armeabi-v7a`) — **64-bit-only devices cannot run
  this**, because the firmware is a 32-bit ARMv5TE image and the port executes
  it natively instead of emulating it.
* Android 8.0+ (`minSdk 26`) for `pthread`/`memfd`-era syscalls used by the
  shim.
* A kernel that permits the syscall-dispatch mechanism used by the patcher
  (see risk 1).

## Importing the firmware

The APK never bundles HP firmware or A:/C: data.  On first launch the activity
shows a prompt with an **导入官方固件** button.

The picker opens with `EXTRA_ALLOW_MULTIPLE`.  The user can select:

* the official update zip (for example
  `HP_Prime_Calculator_Firmware_20260909.zip`);
* the extracted `PRIME_APP.DAT`;
* all three official files at once (`PRIME_APP.DAT`, `PRIME_MASTER.DAT`,
  `PRIME_OS.ROM`).

The app stages the selection in `files/import/` and expands a zip, keeping only
the files an update can contribute.  It then classifies them by magic:

* `PRIME_APP.DAT` (`APDSKXXX.DAT`) — the only file that can supply a runnable
  system.  `primeshim_firmware_import()` (`src/firmware_image.c`) extracts and
  validates the ARM ELF; `FirmwarePackage.java` mounts the FAT16 volume at
  `0x2000` and extracts A:\ (system DLLs, `APPSLIST.*`, `programs/misc/*`).
  `programs/misc/armfir.dat` is published as `A:\dummy.DAT`; it contains the
  Prime Sans font/help database, so leaving it out makes the UI draw no text.
* `PRIME_MASTER.DAT` (`EA656XXX.DAT`) — boot image; reported and ignored.
* `PRIME_OS.ROM` — 1 MiB boot ROM; reported and ignored.
* a raw `armfir.elf` — accepted only as an update for an existing profile;
  a fresh profile needs `PRIME_APP.DAT` because the A: volume is mandatory.

Each firmware hash gets its own profile:

```text
files/profiles/<sha256(elf)[:16]>/
    armfir.elf
    prime_data/A/        extracted from the same PRIME_APP.DAT
    prime_data/C/        empty on first run; restored from a user backup
```

Re-importing the same firmware keeps the profile's C: data.  A new firmware
gets a new profile, so a 2.4.2 A: volume can never be paired with a 15270 ELF.
The A: manifest `APPSLIST.MD5` is verified after extraction; a mismatch aborts
the import instead of publishing a partially extracted A: volume.

The app does not need `PRIME_MASTER.DAT` or `PRIME_OS.ROM`: PrimeShim starts at
the application ELF and patches the firmware's SVCs, unlike a boot-level
emulator that maps the boot ROM as a kernel.

The ELF extraction path is also available from the command line:

```sh
build/pc/primeshim.elf --import-firmware PRIME_APP.DAT armfir.elf
# imported: ok
md5sum armfir.elf   # 67d4785803375677e4dfe8344f55cc1f for firmware 2.4.2
```

## Assets and licensing

HP firmware and the A:/C: volumes are proprietary and are **not** part of the
APK.  The asset tree contains only UI resources (keyboard skins and the fallback
app icon).  `firmware/` and `runtime/prime_data/` remain git-ignored; users must
import their own lawfully obtained update package.

## Key mapping

Letters follow the calculator's QWERTY-style input, and F1–F10 map onto the
top-row function keys:

| Android | Prime | Function |
| --- | --- | --- |
| 0–9 | `0x30,0x59,0x5A,0x33,0x55,0x56,0x57,0x51,0x52,0x53` | digits |
| `+ - * / , . =` | `0xB7 0xB9 0x58 0x54 0x4F 0xB8 0x0D` | arithmetic |
| D-pad | `0x02..0x05` | left/up/right/down |
| `DEL` / `ENTER` / `ESC` | `0x0C` / `0x0D` / `0x01` | editing |
| A–Z | `0x41..0x5A` | var, toolbox, template, sin/cos/tan, ln, log, … |
| F1–F10, MENU, HOME | `0x91,0x93,0xB2,0xB3,0xB4,0xB5,0xB6,0x95,0xB1,0xE047` | Symb, Msg, Plot, Num, View, CAS, Alpha, Help, Apps, Home |
| CTRL | `0x83` | ON |
| BACK | — | handled by Android (leaves the activity) |

Touch is delivered as `UI_EVENT_TYPE_TOUCH_BEGIN/MOVE/END` through the same
queue the physical digitizer uses, so drag gestures work as on hardware.

## Debugging

```sh
adb logcat -s PrimeShim
```

* The firmware's own log level is set by `nativeStart(..., logLevel)`; the
  activity passes `LOG_INFO`.  Use `LOG_DEBUG` or `LOG_TRACE` to see
  unimplemented syscalls and device ioctls.
* `adb shell setprop debug.primeshim.level debug` is *not* wired up yet; change
  the constant in `MainActivity` or call `PrimeShim.nativeSetLogLevel()`.
* The guest prints to stderr, which Android captures under the app's `System.err`
  tag.

## Roadmap

1. ~~Prove or disprove SUD/seccomp availability~~ - replaced by static patching,
   which no longer depends on kernel features.
2. Measure startup cost of the 742 rewritten sites on the calculator.
3. Move composition to `ASurfaceTexture`/`HardwareBuffer` or a GL shader if the
   software scaler shows up in profiles.
4. Settings screen: log level, profile reload, static/fault toggle.
5. Optional: 64-bit build with an embedded ARM interpreter for 64-bit-only phones.
