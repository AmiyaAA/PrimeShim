HOST ?= pc
DATA_DIR ?= runtime/prime_data
FIRMWARE ?= firmware/2025-15270/armfir.elf
KEYBOARD_DEVICE ?= /dev/input/event0
TOUCH_DEVICE ?= /dev/input/event1

CROSS_COMPILE ?= arm-linux-gnueabi-
CC := $(CROSS_COMPILE)gcc
AS := $(CROSS_COMPILE)as
QEMU_ARM ?= qemu-arm
QEMU_GUEST_BASE ?= 0x100000000
ARM_SYSROOT ?= /usr/arm-linux-gnueabi
RUNNER ?=
RUN_ARGS ?=
HOST_CC ?= cc
NATIVE_TEST_TARGET := build/tests/device-test
ARM_PC_SAMPLER_TARGET := build/tools/arm-pc-sampler
ARM_PROCESS_WATCH_TARGET := build/tools/arm-process-watch

BUILD_DIR := build/$(HOST)
TARGET := $(BUILD_DIR)/primeshim.elf

COMMON_C_SRCS := \
	src/device.c \
	src/filesystem.c \
	src/image_shim.c \
	src/keyboard.c \
	src/log.c \
	src/firmware_image.c \
	src/main.c \
	src/mouse.c \
	src/peripheral.c \
	src/platform/lcd_device.c \
	src/platform/lcd_pc.c \
	src/runtime_patcher.c \
	src/runtime_syscalls.c \
	src/syscall_stubs.c \
	src/system_shim.c \
	src/timer.c

ASM_SRCS := src/runtime_dispatch.S
SYSCALL_REGISTRY := $(BUILD_DIR)/generated/syscall_registry.inc

CPPFLAGS := -Iinclude \
	-I$(BUILD_DIR)/generated \
	-DPRIMESHIM_DATA_DIR='"$(DATA_DIR)"' \
	-DPRIMESHIM_FIRMWARE_PATH='"$(FIRMWARE)"' \
	-DPRIMESHIM_KEYBOARD_DEVICE='"$(KEYBOARD_DEVICE)"' \
	-DPRIMESHIM_TOUCH_DEVICE='"$(TOUCH_DEVICE)"'
CFLAGS := -march=armv5te -mabi=aapcs-linux -mfloat-abi=soft -fno-pie -O2 \
	-MMD -MP
ASFLAGS := -march=armv5te -mfloat-abi=soft
LDFLAGS := -march=armv5te -no-pie
LDLIBS := -lpthread

ifeq ($(HOST),pc)
CPPFLAGS += -DPRIMESHIM_HOST_PC=1 -DPRIMESHIM_HOST_DEVICE=0
C_SRCS := $(COMMON_C_SRCS)
SDL_HOST_TARGET := $(BUILD_DIR)/primeshim-sdl
SDL2_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL2_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null)
ALL_TARGETS := $(TARGET) $(SDL_HOST_TARGET)
else ifeq ($(HOST),device)
CPPFLAGS += -DPRIMESHIM_HOST_PC=0 -DPRIMESHIM_HOST_DEVICE=1
CPPFLAGS += -Ithird_party/include
C_SRCS := $(COMMON_C_SRCS) src/usbdhid.c
LDFLAGS += -Lthird_party/lib -Wl,-rpath-link=third_party/lib
LDLIBS += -ldrm -latomic
ALL_TARGETS := $(TARGET)
else
$(error HOST must be either 'pc' or 'device')
endif

C_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SRCS))
ASM_OBJS := $(patsubst %.S,$(BUILD_DIR)/%.o,$(ASM_SRCS))
OBJS := $(C_OBJS) $(ASM_OBJS)

-include $(C_OBJS:.o=.d)

.DEFAULT_GOAL := all

all: $(ALL_TARGETS)

pc:
	$(MAKE) HOST=pc all

device:
	$(MAKE) HOST=device all

check: pc device

test: $(NATIVE_TEST_TARGET)
	$(NATIVE_TEST_TARGET)

arm-pc-sampler: $(ARM_PC_SAMPLER_TARGET)

arm-process-watch: $(ARM_PROCESS_WATCH_TARGET)

$(ARM_PC_SAMPLER_TARGET): tools/arm_pc_sampler.c
	@mkdir -p $(dir $@)
	$(CC) -march=armv5te -mabi=aapcs-linux -mfloat-abi=soft -O2 \
		-Wall -Wextra -Werror $< -o $@

$(ARM_PROCESS_WATCH_TARGET): tools/arm_process_watch.c
	@mkdir -p $(dir $@)
	$(CC) -march=armv5te -mabi=aapcs-linux -mfloat-abi=soft -O2 \
		-Wall -Wextra -Werror $< -o $@

$(NATIVE_TEST_TARGET): tests/device_test.c src/device.c src/log.c \
		include/device.h include/log.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -Iinclude -O2 -Wall -Wextra -Werror -pthread \
		tests/device_test.c src/device.c src/log.c -o $@

run-pc: pc
	$(RUNNER) $(SDL_HOST_TARGET) $(QEMU_ARM) -B $(QEMU_GUEST_BASE) \
		-E LD_LIBRARY_PATH=$(ARM_SYSROOT)/lib \
		-L $(ARM_SYSROOT) \
		build/pc/primeshim.elf --firmware $(FIRMWARE) $(RUN_ARGS)

smoke-runtime: pc
	$(QEMU_ARM) \
		-E LD_LIBRARY_PATH=$(ARM_SYSROOT)/lib \
		-L $(ARM_SYSROOT) \
		build/pc/primeshim.elf --firmware $(FIRMWARE) --patch-only

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJS) $(LDFLAGS) $(LDLIBS) -o $@

$(SDL_HOST_TARGET): src/host/sdl_host.c src/log.c include/host_ipc.h include/log.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -Iinclude $(SDL2_CFLAGS) -O2 -Wall -Wextra \
		src/host/sdl_host.c src/log.c $(SDL2_LIBS) -lpthread -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/src/runtime_syscalls.o: $(SYSCALL_REGISTRY)

$(SYSCALL_REGISTRY): tools/interruptmap.txt tools/generate_syscall_registry.py
	@mkdir -p $(dir $@)
	python3 tools/generate_syscall_registry.py $< $@

clean:
	rm -rf build

print-config:
	@echo "HOST=$(HOST)"
	@echo "TARGET=$(TARGET)"
	@echo "DATA_DIR=$(DATA_DIR)"
	@echo "CPPFLAGS=$(CPPFLAGS)"

.PHONY: all pc device check test arm-pc-sampler arm-process-watch run-pc smoke-runtime clean print-config
