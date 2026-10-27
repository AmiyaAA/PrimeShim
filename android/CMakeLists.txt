#
# PrimeShim Android native library.
#
# Builds the whole runtime as libprimeshim.so so Java can start it in-process.
# Only platform-neutral sources plus the Android backend are compiled: lcd_pc.c
# (SDL IPC), lcd_device.c (DRM/evdev) and usbdhid.c (ConfigFS) are excluded.
#
cmake_minimum_required(VERSION 3.22.1)
project(primeshim C ASM)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# src/runtime_syscalls.c includes a table generated from tools/interruptmap.txt
# (the Makefile builds do this in tools/jcc; CMake has to do it here).
find_program(PYTHON3 NAMES python3 python)
if(NOT PYTHON3)
    message(FATAL_ERROR "python3 is required to generate the syscall registry")
endif()
set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_DIR}")
execute_process(
    COMMAND "${PYTHON3}" "${REPO_ROOT}/tools/generate_syscall_registry.py"
            "${REPO_ROOT}/tools/interruptmap.txt"
            "${GEN_DIR}/syscall_registry.inc"
    RESULT_VARIABLE SYSCALL_GEN_RESULT)
if(NOT SYSCALL_GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "syscall registry generation failed (${SYSCALL_GEN_RESULT})")
endif()

add_library(primeshim SHARED
    ${REPO_ROOT}/src/device.c
    ${REPO_ROOT}/src/filesystem.c
    ${REPO_ROOT}/src/image_shim.c
    ${REPO_ROOT}/src/firmware_image.c
    ${REPO_ROOT}/src/keyboard.c
    ${REPO_ROOT}/src/log.c
    ${REPO_ROOT}/src/main.c
    ${REPO_ROOT}/src/mouse.c
    ${REPO_ROOT}/src/peripheral.c
    ${REPO_ROOT}/src/platform/lcd_android.c
    ${REPO_ROOT}/src/runtime_dispatch.S
    ${REPO_ROOT}/src/runtime_patcher.c
    ${REPO_ROOT}/src/runtime_syscalls.c
    ${REPO_ROOT}/src/syscall_stubs.c
    ${REPO_ROOT}/src/system_shim.c
    ${REPO_ROOT}/src/timer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/jni/address_window.c
    ${CMAKE_CURRENT_SOURCE_DIR}/jni/log_capture.c
    ${CMAKE_CURRENT_SOURCE_DIR}/jni/primeshim_jni.c)

# main.c becomes a plain function so the JNI layer can run the guest on a thread.
set_source_files_properties(${REPO_ROOT}/src/main.c
    PROPERTIES COMPILE_DEFINITIONS "main=primeshim_main")

target_include_directories(primeshim PRIVATE
    ${REPO_ROOT}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/jni
    ${GEN_DIR})

target_compile_definitions(primeshim PRIVATE
    PRIMESHIM_HOST_PC=0
    PRIMESHIM_HOST_DEVICE=0
    PRIMESHIM_HOST_ANDROID=1
    PRIMESHIM_DATA_DIR="prime_data")

# The firmware is ARMv5TE soft-float; keep the shim at ARMv7 softfp so integer
# and pointer ABIs match what the patcher and trampolines assume.
target_compile_options(primeshim PRIVATE
    -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=softfp
    -fno-strict-aliasing -fvisibility=hidden
    -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(primeshim android log m)
