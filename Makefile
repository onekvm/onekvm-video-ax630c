# Host ABI smoke and the aarch64 musl backend installed on NanoKVM Pro.
CC ?= gcc
CROSS ?= /home/samlm/projects/onekvm/onekvm-distro/.oe/build-nanokvm-pro/tmp/sysroots-components/x86_64/gcc-cross-aarch64/usr/bin/aarch64-oe-linux-musl/aarch64-oe-linux-musl-gcc
INC := -I/home/samlm/projects/onekvm/onekvm-generic-v4l2/include -Ithird_party/vcenc-ewl
CFLAGS := -O2 -fPIC -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden $(INC)
LDFLAGS := -shared -pthread

.PHONY: all host device smoke clean

all: host device

host: build/ax630c.so build/abi-smoke build/no-signal-test

device: build/ax630c-aarch64.so build/ax630c-once

build:
	mkdir -p build

build/ax630c.so: src/ax630c_video.c src/ax_no_signal.c src/no_signal_frames.inc | build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/ax630c_video.c src/ax_no_signal.c

build/abi-smoke: tests/abi-smoke.c build/ax630c.so | build
	$(CC) -O2 -Wall $(INC) -o $@ tests/abi-smoke.c -ldl

build/ax630c-aarch64.so: src/ax630c_video.c src/ax_no_signal.c src/no_signal_frames.inc | build
	$(CROSS) $(CFLAGS) $(LDFLAGS) -o $@ src/ax630c_video.c src/ax_no_signal.c

build/ax630c-once: src/ax630c_video.c src/ax_no_signal.c src/no_signal_frames.inc | build
	$(CROSS) $(CFLAGS) -DAX630C_STANDALONE -o $@ src/ax630c_video.c src/ax_no_signal.c -pthread

build/no-signal-test: tests/no-signal-test.c src/ax_no_signal.c src/no_signal_frames.inc | build
	$(CC) -O2 -Wall -Wextra -o $@ tests/no-signal-test.c src/ax_no_signal.c

smoke: host
	./build/abi-smoke ./build/ax630c.so
	./build/no-signal-test

clean:
	rm -rf build
