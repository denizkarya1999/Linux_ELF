# Linux_ELF

Small C programs and experiments for exploring **ELF binary compatibility between GNU/Linux and Android**.

The first program, `kernel_info`, reports the OS name and version, kernel release and build, and machine architecture. Android builds also report the API level. The same source builds for either platform.

## What this project explores

ELF is an executable file format. Sharing that format and a Linux kernel does **not** automatically make GNU/Linux and Android binaries interchangeable. Compatibility also depends on the CPU instruction set, ABI, dynamic loader, library dependencies, kernel interfaces, and execution permissions. See the [Android ABI documentation](https://developer.android.com/ndk/guides/abis).

Android uses Bionic as its C library; GNU/Linux builds on Ubuntu normally use glibc. Their loaders and library interfaces differ. See the [Bionic overview](https://android.googlesource.com/platform/bionic/+/main/README.md).

The current program demonstrates **source portability** through separate builds. It does not provide a single universal executable or emulate another CPU.

## Files

- `kernel_info.c` — commented source for Android and GNU/Linux.
- `Makefile` — build commands for the host and Android.
- `.gitignore` — keeps generated executables out of Git.

## Build and run on GNU/Linux

Requirements: a C compiler and GNU Make. `file` and `readelf` are useful for inspecting the results (`readelf` is part of binutils).

```bash
git clone https://github.com/denizkarya1999/Linux_ELF.git
cd Linux_ELF
make linux
./kernel_info
```

Or compile directly:

```bash
cc -std=c11 -Wall -Wextra -O2 -fPIE -pie kernel_info.c -o kernel_info
./kernel_info
```

This builds for the compiler's target architecture. For example, building with the native compiler on an x86-64 Ubuntu PC produces an x86-64 executable.

## Build and run on Android

Requirements: the Android NDK, ADB, and a phone with USB debugging enabled and the computer authorized.

Set the NDK path to the version installed on your computer. This example uses NDK r28c on an x86-64 Linux host:

```bash
export ANDROID_NDK_HOME="$HOME/Android/Sdk/ndk/28.2.13676358"
make android
adb push kernel_info_android /data/local/tmp/kernel_info
adb shell chmod 755 /data/local/tmp/kernel_info
adb shell /data/local/tmp/kernel_info
```

The default Android build targets `arm64-v8a` (AArch64) and API level 21 or later. Check the device's ABI and API level:

```bash
adb shell getprop ro.product.cpu.abi
adb shell getprop ro.build.version.sdk
```

For an x86-64 Android emulator, use:

```bash
make android ANDROID_TARGET=x86_64-linux-android
```

`ANDROID_API` and `NDK_HOST_TAG` can also be overridden. The host tag identifies the computer running the compiler, while `ANDROID_TARGET` identifies the Android target. See the [NDK toolchain instructions](https://developer.android.com/ndk/guides/other_build_systems).

Use `/data/local/tmp` for ADB execution. Shared storage such as `/sdcard` or `/storage/emulated/0` is mounted with execution restrictions; storing an ELF there does not make it runnable. See [Android's shared-storage mount code](https://android.googlesource.com/platform/system/core/+/android16-qpr2-release/sdcard/sdcard.cpp).

## Inspect binary compatibility

Build both targets first, then inspect them without executing them:

```bash
file kernel_info kernel_info_android
readelf -h kernel_info
readelf -h kernel_info_android
readelf -l kernel_info
readelf -l kernel_info_android
readelf -d kernel_info
readelf -d kernel_info_android
```

Compare the `Machine` field, requested program interpreter, and `NEEDED` library entries. For the current Ubuntu x86-64 and Android ARM64 builds:

| Property | Ubuntu build | Android build |
| --- | --- | --- |
| File format | ELF64, position-independent executable | ELF64, position-independent executable |
| CPU | x86-64 | AArch64 |
| Interpreter | `/lib64/ld-linux-x86-64.so.2` | `/system/bin/linker64` |
| C library | glibc | Bionic |

These builds differ in both CPU architecture and runtime. To investigate runtime compatibility separately, build for matching CPUs, such as GNU/Linux AArch64 and Android AArch64. A successful native run alone does not establish cross-platform binary compatibility.

Record the build target, device ABI, ELF interpreter, dependencies, exit status, and complete output for each experiment. An `Exec format error` can indicate a CPU or ELF format mismatch; `No such file or directory` for an existing executable can mean its interpreter is missing; `Permission denied` can come from permissions, mount restrictions, or security policy. Inspect the evidence before drawing a conclusion.

Static linking may remove a dynamic-loader dependency, but does not guarantee compatibility with another CPU, kernel, or security policy.

## OS information

On GNU/Linux, the program reads `NAME`, `VERSION`, and, if needed, `VERSION_ID` from `/etc/os-release`, falling back to `/usr/lib/os-release`. Missing information uses a fallback label. See the [os-release specification](https://www.freedesktop.org/software/systemd/man/latest/os-release.html).

On Android, it reads the release and API level through system properties. On both platforms, `uname()` provides the kernel and machine information. The reported machine architecture describes the kernel's view, not necessarily the executable's ABI; inspect the ELF header for that.

## Validation so far

- GNU/Linux: compiled with warnings treated as errors and ran successfully on the development Ubuntu x86-64 machine.
- Android: compiled with NDK r28c for ARM64/API 21 and deployed to a Pixel 8a. On-device execution is left to the user; no cross-platform runtime success is claimed.

## Hashtags

#ELF #GNULinux #Linux #Android #BinaryCompatibility #CProgramming #AndroidNDK #ABI #SystemsProgramming
