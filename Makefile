CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O2
ANDROID_API ?= 21
ANDROID_TARGET ?= aarch64-linux-android
NDK_HOST_TAG ?= linux-x86_64
ANDROID_CC = $(ANDROID_NDK_HOME)/toolchains/llvm/prebuilt/$(NDK_HOST_TAG)/bin/$(ANDROID_TARGET)$(ANDROID_API)-clang

PROGRAMS = kernel_info elf_info

.PHONY: all linux android test clean
all: linux

# Build for the host, or use CC to select a GNU/Linux cross-compiler.
linux: $(addsuffix .c,$(PROGRAMS))
	@set -e; for program in $(PROGRAMS); do \
		$(CC) $(CPPFLAGS) $(CFLAGS) -fPIE -pie "$$program.c" $(LDFLAGS) -o "$$program"; \
	done

# The default Android target is ARM64, with API level 21 as the minimum.
android: $(addsuffix .c,$(PROGRAMS))
	@test -n "$(ANDROID_NDK_HOME)" || { echo "Set ANDROID_NDK_HOME to your Android NDK directory."; exit 1; }
	@test -x "$(ANDROID_CC)" || { echo "Android compiler not found: $(ANDROID_CC)"; exit 1; }
	@set -e; for program in $(PROGRAMS); do \
		"$(ANDROID_CC)" $(CPPFLAGS) $(CFLAGS) -fPIE -pie "$$program.c" $(LDFLAGS) -o "$${program}_android"; \
	done

test: linux
	python3 tests/test_elf_info.py ./elf_info

clean:
	rm -f $(PROGRAMS) $(addsuffix _android,$(PROGRAMS))
