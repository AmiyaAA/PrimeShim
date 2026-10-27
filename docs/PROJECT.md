# PrimeShim development guide

## What this project is

PrimeShim is a compatibility layer, not a CPU emulator. The original HP Prime ARM ELF is mapped at its requested virtual addresses when PrimeShim starts. Runtime patch modules rewrite registered firmware software interrupts and peripheral accesses into branches to Linux-side handlers. On an x86-64 PC, QEMU user mode supplies ARM CPU execution; on the custom calculator Linux system, the binary runs natively.

The supported firmware is HP Prime OS `15270` (2025-01-31). Its code expects ARMv5TE, soft-float EABI, fixed virtual addresses, 32-bit pointers, and several memory-mapped hardware registers. Treat those details as ABI requirements, not ordinary implementation choices.

## Repository map

```text
PrimeShim/
├── Makefile                         build profiles and source manifest
├── include/
│   ├── platform.h                   compile-time host selection/configuration
│   ├── ui.h                         firmware UI event ABI
│   └── usbdhid.h                    device-only USB HID interface
├── src/
│   ├── main.c                       runtime loader orchestration and startup
│   ├── runtime_patcher.c            ELF loader, patch discovery, trampolines
│   ├── runtime_dispatch.S           small peripheral callback bridge
│   ├── runtime_syscalls.c           generated syscall registration module
│   ├── peripheral.c                 host I/O driver registry and lifecycle
│   ├── device.c                     guest device handles and ioctl dispatch
│   ├── log.c                        leveled, component-based diagnostics
│   ├── syscall_stubs.c              weak fallback for unimplemented syscalls
│   ├── system_shim.c                threads, time, events, device calls
│   ├── filesystem.c                 DOS-like VFS, INI and firmware loader calls
│   ├── keyboard.c / mouse.c         input translation and device drivers
│   ├── host/sdl_host.c              native SDL2/QEMU IPC launcher
│   ├── timer.c                      emulated hardware timer and interrupt bridge
│   ├── usbdhid.c                    device-only ConfigFS HID gadget
│   └── platform/
│       ├── lcd_pc.c                 ARM-side shared framebuffer/input IPC
│       └── lcd_device.c             DRM display with fbdev fallback
├── firmware/2025-15270/
│   ├── armfir.elf / armfir.dat      retained 2025 firmware inputs
│   └── text.asm/data.asm/linker.ld  legacy generated reference artifacts
├── runtime/prime_data/              virtual A: and C: drive contents
├── third_party/                     ARM libdrm headers and shared library
└── tools/                            generators and ARM profiling aids
```

Generated build products live under `build/<host>/` and are ignored by Git.

## Host profiles

The Makefile always defines exactly one profile through `include/platform.h`:

| Behavior | `HOST=pc` | `HOST=device` |
| --- | --- | --- |
| CPU | ARM under QEMU user mode | native ARM |
| Display | native SDL2 window over shared memory | DRM zero-copy/render thread, fbdev fallback |
| Keyboard | SDL events over a pipe | matrix keypad, default `/dev/input/event0` |
| Pointer | SDL mouse/touch events over a pipe | multitouch, default `/dev/input/event1` |
| USB | requests are safely ignored | ConfigFS HID gadget |
| Power off | safely ignored | writes `mem` to `/sys/power/state` |
| Low page mapping | QEMU guest-base offset, RAM fallback | four-byte RAM fallback |

Use `PRIMESHIM_HOST_PC` and `PRIMESHIM_HOST_DEVICE` only for behavior that truly differs between hosts. RTOS translations belong in the common files. Device paths should remain build variables rather than hard-coded local edits.

## Peripheral driver bus

I/O backends expose a `primeshim_peripheral_driver_t` and register it with
`PRIMESHIM_REGISTER_PERIPHERAL_DRIVER`. A driver declares one or more exclusive
classes, whether it is required, and probe/start/stop callbacks. `main.c` starts
the bus after firmware preparation and compatibility mappings; shutdown runs
active drivers in reverse registration order.

The PC backend claims display, keyboard, and pointer as one unit because all
three share the SDL host connection. The native x86-64 SDL process owns the
window and launches the ARM QEMU process. The two processes share pixels via an
inherited `memfd` and send fixed-width input messages through an inherited pipe,
so the ARM sysroot does not need SDL2. Device DRM/fbdev, matrix keypad,
touchscreen, and USB HID are separate drivers and can evolve independently.

## Guest device registry

The peripheral bus owns host backend lifecycle; the device registry in
`include/device.h` independently models BESTA guest devices opened through
`CreateFile`. Drivers register a stable name and open/close/io-control
callbacks. Handles retain both the selected driver and its per-open context,
so `DeviceIoControl` contains no device-name branches. Device handlers return a
signed 32-bit result; the ABI wrapper also writes those exact bits to the
firmware result pointer.

## Logging

Use `PS_LOG_ERROR`, `PS_LOG_WARN`, `PS_LOG_INFO`, `PS_LOG_DEBUG`, and
`PS_LOG_TRACE` with a short component name. The default threshold is `warn`;
`PRIMESHIM_LOG_LEVEL` or `--log-level` selects another threshold. High-volume
unimplemented-handler messages belong at debug; fine-grained filesystem,
event, input, and ioctl activity belongs at trace. The level is configured once
during startup, before worker threads are created. Macros use a plain inline
comparison, so suppressed hot-path logs do not lock or evaluate their
arguments. Do not add unconditional diagnostic `printf` calls.

## Firmware-to-Linux call path

1. `runtime_patcher.c` validates `armfir.elf`, atomically creates or refreshes
   the adjacent `armfir.elf.map`, maps it privately at the ELF virtual
   addresses, and records executable sections. Clean pages remain file-backed;
   BSS, runtime writes, and patched code pages become private on demand. If the
   map cannot be created it falls back to an anonymous PT_LOAD copy.
2. `tools/generate_syscall_registry.py` turns `tools/interruptmap.txt` into the syscall module's compiled registration table. This does not alter the firmware.
3. By default no firmware instruction is scanned or rewritten while the ELF
   loads.  `runtime_patcher_static_patch_syscalls()` is an opt-in alternative
   (default on Android): it rewrites every registered `svc` up front so no
   signal delivery is needed, and only accepts `cond == AL` sites so literal
   pools are never touched. A registered A32 SVC is patched only after the executed instruction is reported by Syscall User Dispatch, seccomp `SIGSYS`, or QEMU's `SIGILL` fallback.
4. SUD inclusive mode covers only the mapped firmware interval, so native shim and libc calls remain direct with no selector switch at handler boundaries. A binding marked `RUNTIME_SYSCALL_BESTA_FRAME` makes its trampoline execute `add sp, sp, #8`, then tail-dispatch to the C handler with the original LR and stack arguments.
5. Peripheral modules register address ranges and read/write callbacks. An A32 `ldr`/`str` is patched only when it actually faults on a registered effective address. The generated trampoline compares the live address operands with the values that caused that fault and calls the already-selected binding directly. A mismatch restores flags/registers and executes the original instruction, allowing a later registered address to fault and specialize that copied instruction recursively.
6. A fixed trampoline arena is allocated immediately after the loaded image. This keeps firmware branches inside ARM's +/-32 MiB range and also permits recursive specialization if an original access copied into a trampoline later faults on a registered address.
7. `src/syscall_stubs.c` provides weak fallback symbols. Strong implementations in the normal shim modules override them when the compiled registry takes their addresses.

This strong-over-weak rule remains the central syscall implementation mechanism. Runtime registration is independently configurable: a module chooses `B` or `BL`, marks whether the BESTA stack frame must be removed, and may replace an earlier binding by registering the same SWI number again.

## Common development tasks

### Implement a missing syscall

1. Find the syscall name/number in `tools/interruptmap.txt` and its weak stub in `src/syscall_stubs.c`.
2. Reverse the exact 32-bit ARM ABI signature. Pointer layout, packing, signedness, and return width matter.
3. Add one strong `SDKLIB_*` implementation to the relevant common or platform file.
4. Build both profiles with `make check` even if the behavior is device-specific.
5. Exercise it with firmware and keep logging concise enough not to change timing substantially.

Do not delete the weak stub: it documents coverage and is intentionally overridden at link time.

### Load another firmware image

Use `make run-pc FIRMWARE=/path/to/armfir.elf`, or pass `--firmware` directly to the executable. No generated assembly or firmware-specific relink is required. A new firmware is still a deliberate ABI port: inspect its entry point and mappings, run `make smoke-runtime FIRMWARE=...` to verify that fault-time patching can be armed, and audit syscall signatures and peripheral registrations.

### Change the virtual filesystem

BESTA drive paths are mapped below `runtime/prime_data`: `A:\\...` becomes `runtime/prime_data/A/...`, and `C:\\...` becomes `runtime/prime_data/C/...`. The process must start in the repository root unless `DATA_DIR` is overridden at build time:

```sh
make HOST=pc DATA_DIR=/absolute/path/to/prime_data
```

Firmware execution writes settings and app data. Keep pristine fixtures separate if a reproducible test is needed.

## Safety and debugging notes

- The runtime firmware and trampoline mappings are intentionally read/write/execute. The shim executable itself is a normal low-address non-PIE ARM executable.
- The ELF loader refuses to overwrite an occupied requested range. Hardware compatibility mappings in `main.c` still use `MAP_FIXED`; changing those addresses can overwrite process mappings or break firmware assumptions.
- Registered peripheral faults are decoded and dispatched by the runtime patcher. Any other `SIGSEGV` keeps the prior/default action. `--continue-on-segfault` opt-in restores the old unsafe behavior for firmware A32 code when debugging.
- The device profile can suspend the machine, change backlight brightness, configure USB gadget state, and access DRM/input devices. Run it only on the intended calculator Linux environment.
- The PC profile blocks firmware power-off and USB gadget actions. Its SDL
  backend requires no raw framebuffer or evdev access.
- `tools/gdb/segf.gdb` expects a QEMU GDB server on port 1234 and the PC build at `build/pc/primeshim.elf`.

## Rules for future automated changes

- Keep the 2025 firmware artifacts as one matched set.
- Keep common syscall behavior out of host branches unless Linux hardware behavior truly differs.
- Prefer named structures and fixed-width integer types for reverse-engineered ABI data.
- Document unknown fields and observed offsets; do not “clean up” packed layouts without evidence.
- Preserve weak stubs until a strong implementation is verified.
- Build both profiles after every source or Makefile change.
- Do not commit `build/`, logs, core dumps, or mutated runtime settings.
