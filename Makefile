# Host checks only. Device packages are built by onekvm-distro/scripts/kas.sh.
CC ?= gcc
INC := -I../onekvm-generic-v4l2/include -Ithird_party/vcenc-ewl
CFLAGS := -O2 -fPIC -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden $(INC)
LDFLAGS := -shared -pthread

.PHONY: all host smoke clean

all: host

host: build/ax630c.so build/abi-smoke build/no-signal-test build/display-test

build:
	mkdir -p build

build/ax630c.so: src/ax630c_video.c src/ax_no_signal.c src/ax_display.c src/ax_display.h src/no_signal_frames.inc | build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/ax630c_video.c src/ax_no_signal.c src/ax_display.c

build/abi-smoke: tests/abi-smoke.c build/ax630c.so | build
	$(CC) -O2 -Wall $(INC) -o $@ tests/abi-smoke.c -ldl

build/no-signal-test: tests/no-signal-test.c src/ax_no_signal.c src/ax_display.c src/ax_display.h src/no_signal_frames.inc | build
	$(CC) -O2 -Wall -Wextra -o $@ tests/no-signal-test.c src/ax_no_signal.c

build/display-test: tests/display-test.c src/ax_display.c src/ax_display.h | build
	$(CC) -O2 -Wall -Wextra -Isrc -o $@ tests/display-test.c src/ax_display.c

smoke: host
	./build/abi-smoke ./build/ax630c.so
	./build/no-signal-test
	./build/display-test

clean:
	rm -rf build
