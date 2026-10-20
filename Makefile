# Host checks only. Device packages are built by onekvm-distro/scripts/kas.sh.
CC ?= gcc
INC := -Ibuild/include -I../onekvm-core/include -I../onekvm-generic-v4l2/include -Ithird_party/vcenc-ewl
CFLAGS := -O2 -fPIC -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden $(INC)
LDFLAGS := -shared -pthread

.PHONY: all host smoke clean

all: host

host: build/ax630c.so build/abi-smoke build/no-signal-test build/display-test build/display-capture-test build/hevc-header-test

build:
	mkdir -p build

build/include/linux/axera_eip130.h: ../linux-6.18-ax620e/include/uapi/linux/axera_eip130.h | build
	mkdir -p build/include/linux
	cp $< $@

TRANSFORM := ../onekvm-generic-v4l2/src/v4l2_transform.c

build/tdp-v4l2-test: tests/tdp-v4l2-test.c $(TRANSFORM) | build
	$(CC) -O2 -Wall -Wextra $(INC) -o $@ $^

build/ax630c.so: build/include/linux/axera_eip130.h $(TRANSFORM) src/ax630c_video.c src/ax_no_signal.c src/ax_display.c src/ax_tdp.c src/ax_tdp.h src/ax_crypto.c src/ax_display.h src/no_signal_frames.inc $(wildcard third_party/vcenc-ewl/*.h) ../onekvm-core/include/onekvm/crypto_backend_v1.h ../onekvm-core/include/onekvm/video_backend_v1.h | build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/ax630c_video.c src/ax_no_signal.c src/ax_display.c src/ax_tdp.c src/ax_crypto.c $(TRANSFORM) -lcrypto

build/abi-smoke: tests/abi-smoke.c build/ax630c.so | build
	$(CC) -O2 -Wall $(INC) -o $@ tests/abi-smoke.c -ldl

build/crypto-abi-test: tests/crypto-abi-test.c ../onekvm-core/include/onekvm/crypto_backend_v1.h | build
	$(CC) -O2 -Wall -Wextra $(INC) -o $@ tests/crypto-abi-test.c -ldl -lcrypto -pthread

build/eip130-ioctl-test: tests/eip130-ioctl-test.c build/include/linux/axera_eip130.h | build
	$(CC) -O2 -Wall -Wextra $(INC) -o $@ tests/eip130-ioctl-test.c -pthread

build/no-signal-test: tests/no-signal-test.c src/ax_no_signal.c src/ax_display.c src/ax_display.h src/no_signal_frames.inc | build
	$(CC) -O2 -Wall -Wextra -o $@ tests/no-signal-test.c src/ax_no_signal.c

build/display-test: tests/display-test.c src/ax_display.c src/ax_display.h | build
	$(CC) -O2 -Wall -Wextra -Isrc -o $@ tests/display-test.c src/ax_display.c

build/display-capture-test: $(TRANSFORM) tests/display-capture-test.c src/ax630c_video.c src/ax_no_signal.c src/ax_display.c src/ax_tdp.c src/ax_tdp.h $(wildcard third_party/vcenc-ewl/*.h) | build
	$(CC) $(CFLAGS) -pthread -Wl,--wrap=poll -Wl,--wrap=ioctl -Wl,--wrap=ax_tdp_convert -o $@ tests/display-capture-test.c src/ax_no_signal.c src/ax_display.c src/ax_tdp.c $(TRANSFORM)

build/hevc-header-test: tests/hevc-header-test.c third_party/vcenc-ewl/vcenc_hevc_header.h third_party/vcenc-ewl/vcenc_header.h | build
	$(CC) -O2 -Wall $(INC) -o $@ tests/hevc-header-test.c

smoke: host
	./build/abi-smoke ./build/ax630c.so
	./build/no-signal-test
	./build/display-test
	./build/display-capture-test
	./build/hevc-header-test

clean:
	rm -rf build
