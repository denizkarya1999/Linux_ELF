CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O2
ANDROID_API ?= 21
ANDROID_TARGET ?= aarch64-linux-android
NDK_HOST_TAG ?= linux-x86_64
ANDROID_CC = $(ANDROID_NDK_HOME)/toolchains/llvm/prebuilt/$(NDK_HOST_TAG)/bin/$(ANDROID_TARGET)$(ANDROID_API)-clang

.PHONY: all linux android clean
all: linux

# Build for the host, or use CC to select a GNU/Linux cross-compiler.
linux: kernel_info.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIE -pie $< $(LDFLAGS) -o kernel_info

# The default Android target is ARM64, with API level 21 as the minimum.
android: kernel_info.c
	@test -n "$(ANDROID_NDK_HOME)" || { echo "Set ANDROID_NDK_HOME to your Android NDK directory."; exit 1; }
	@test -x "$(ANDROID_CC)" || { echo "Android compiler not found: $(ANDROID_CC)"; exit 1; }
	"$(ANDROID_CC)" $(CPPFLAGS) $(CFLAGS) -fPIE -pie $< $(LDFLAGS) -o kernel_info_android

clean:
	rm -f kernel_info kernel_info_android
