# Host checks only. Device packages are built by onekvm-distro/scripts/kas.sh.
CC ?= gcc
INC := -Ibuild/include -I../onekvm-core/include -I../onekvm-generic-v4l2/include -Ithird_party/vcenc-ewl
JPEG_CFLAGS ?= $(shell pkg-config --cflags libjpeg 2>/dev/null)
JPEG_LIBS ?= -ljpeg
CFLAGS := -O2 -fPIC -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden $(INC) $(JPEG_CFLAGS)
LDFLAGS := -shared -pthread

.PHONY: all host smoke clean

all: host

host: build/ax630c.so build/abi-smoke build/no-signal-test build/display-test build/display-capture-test build/hevc-header-test build/edid-test build/rate-control-test build/vcenc-rc-test build/snapshot-test build/pacing-capture-test build/encoder-buffer-test build/snapshot-conversion-test build/tdp-cache-test build/input-format-test build/capture-teardown-test

build:
	mkdir -p build

build/include/linux/axera_eip130.h: ../linux-6.18-ax620e/include/uapi/linux/axera_eip130.h | build
	mkdir -p build/include/linux
	cp $< $@

TRANSFORM := ../onekvm-generic-v4l2/src/v4l2_transform.c

build/tdp-v4l2-test: tests/tdp-v4l2-test.c $(TRANSFORM) | build
	$(CC) -O2 -Wall -Wextra $(INC) -o $@ $^

build/ax630c.so: Makefile $(TRANSFORM) src/ax630c_video.c src/ax_pacing.h src/ax_rate.c src/ax_rate.h src/ax_edid.c src/ax_edid.h src/ax_no_signal.c src/ax_display.c src/ax_tdp.c src/ax_snapshot.c src/ax_snapshot.h src/ax_tdp.h src/ax_display.h src/no_signal_frames.inc $(wildcard third_party/vcenc-ewl/*.h) ../onekvm-core/include/onekvm/video_backend_v1.h | build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/ax630c_video.c src/ax_rate.c src/ax_edid.c src/ax_no_signal.c src/ax_display.c src/ax_tdp.c src/ax_snapshot.c $(TRANSFORM) -lm $(JPEG_LIBS)

build/abi-smoke: tests/abi-smoke.c build/ax630c.so | build
	$(CC) -O2 -Wall $(INC) -o $@ tests/abi-smoke.c -ldl

build/crypto-abi-test: tests/crypto-abi-test.c ../onekvm-core/include/onekvm/crypto_backend_v1.h | build
	$(CC) -O2 -Wall -Wextra $(INC) -o $@ tests/crypto-abi-test.c -ldl -lcrypto -pthread

# Legacy hardware implementation tests use a separate mock DSO. The production
# video backend does not compile or export the crypto provider.
build/crypto-mock.so: src/ax_crypto.c tests/crypto-mock.c build/include/linux/axera_eip130.h ../onekvm-core/include/onekvm/crypto_backend_v1.h | build
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--wrap=open -Wl,--wrap=close -Wl,--wrap=ioctl -o $@ src/ax_crypto.c tests/crypto-mock.c -lcrypto

.PHONY: crypto-check
crypto-check: build/crypto-abi-test build/crypto-mock.so
	./build/crypto-abi-test ./build/crypto-mock.so

build/eip130-ioctl-test: tests/eip130-ioctl-test.c build/include/linux/axera_eip130.h | build
	$(CC) -O2 -Wall -Wextra $(INC) -o $@ tests/eip130-ioctl-test.c -pthread

build/no-signal-test: tests/no-signal-test.c src/ax_no_signal.c src/ax_display.c src/ax_display.h src/no_signal_frames.inc | build
	$(CC) -O2 -Wall -Wextra -o $@ tests/no-signal-test.c src/ax_no_signal.c

build/display-test: tests/display-test.c src/ax_display.c src/ax_display.h | build
	$(CC) -O2 -Wall -Wextra -Isrc -o $@ tests/display-test.c src/ax_display.c

build/display-capture-test: $(TRANSFORM) tests/display-capture-test.c src/ax630c_video.c src/ax_pacing.h src/ax_rate.c src/ax_rate.h src/ax_edid.c src/ax_edid.h src/ax_no_signal.c src/ax_display.c src/ax_tdp.c src/ax_snapshot.c src/ax_snapshot.h src/ax_tdp.h $(wildcard third_party/vcenc-ewl/*.h) | build
	$(CC) $(CFLAGS) -pthread -Wl,--wrap=poll -Wl,--wrap=ioctl -Wl,--wrap=ax_tdp_convert -o $@ tests/display-capture-test.c src/ax_rate.c src/ax_edid.c src/ax_no_signal.c src/ax_display.c src/ax_tdp.c src/ax_snapshot.c $(TRANSFORM) -lm $(JPEG_LIBS)

build/snapshot-test: tests/snapshot-test.c src/ax630c_video.c src/ax_pacing.h src/ax_snapshot.c src/ax_snapshot.h src/ax_tdp.c src/ax_tdp.h src/ax_rate.c src/ax_edid.c src/ax_no_signal.c src/ax_display.c $(TRANSFORM) | build
	$(CC) $(CFLAGS) -pthread -Wl,--wrap=poll -Wl,--wrap=ioctl -Wl,--wrap=ax_tdp_snapshot_acquire -Wl,--wrap=ax_tdp_snapshot_release -o $@ tests/snapshot-test.c src/ax_snapshot.c src/ax_tdp.c src/ax_rate.c src/ax_edid.c src/ax_no_signal.c src/ax_display.c $(TRANSFORM) -lm $(JPEG_LIBS)

build/pacing-capture-test: tests/pacing-capture-test.c src/ax630c_video.c src/ax_pacing.h src/ax_snapshot.c src/ax_tdp.c src/ax_rate.c src/ax_edid.c src/ax_no_signal.c src/ax_display.c $(TRANSFORM) | build
	$(CC) $(CFLAGS) -pthread -Wl,--wrap=poll -Wl,--wrap=ioctl -o $@ tests/pacing-capture-test.c src/ax_snapshot.c src/ax_tdp.c src/ax_rate.c src/ax_edid.c src/ax_no_signal.c src/ax_display.c $(TRANSFORM) -lm $(JPEG_LIBS)

build/encoder-buffer-test: tests/encoder-buffer-test.c src/ax630c_video.c src/ax_pacing.h src/ax_snapshot.c src/ax_tdp.c src/ax_rate.c src/ax_edid.c src/ax_no_signal.c src/ax_display.c $(TRANSFORM) | build
	$(CC) $(CFLAGS) -pthread -Wl,--wrap=poll -Wl,--wrap=ioctl -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -o $@ tests/encoder-buffer-test.c src/ax_snapshot.c src/ax_tdp.c src/ax_rate.c src/ax_edid.c src/ax_no_signal.c src/ax_display.c $(TRANSFORM) -lm $(JPEG_LIBS)

build/tdp-cache-test: tests/tdp-cache-test.c src/ax_tdp.c src/ax_tdp.h | build
	$(CC) $(CFLAGS) -o $@ tests/tdp-cache-test.c src/ax_tdp.c

build/input-format-test: tests/input-format-test.c src/ax630c_video.c src/ax_pacing.h src/ax_snapshot.c src/ax_tdp.c src/ax_rate.c src/ax_edid.c src/ax_no_signal.c src/ax_display.c $(TRANSFORM) | build
	$(CC) $(CFLAGS) -pthread -Wl,--wrap=fopen -Wl,--wrap=open -o $@ tests/input-format-test.c src/ax_snapshot.c src/ax_tdp.c src/ax_rate.c src/ax_edid.c src/ax_no_signal.c src/ax_display.c $(TRANSFORM) -lm $(JPEG_LIBS)

build/capture-teardown-test: tests/capture-teardown-test.c src/ax630c_video.c src/ax_pacing.h src/ax_snapshot.c src/ax_tdp.c src/ax_rate.c src/ax_edid.c src/ax_no_signal.c src/ax_display.c $(TRANSFORM) | build
	$(CC) $(CFLAGS) -pthread -Wl,--wrap=onekvm_v4l2_transform_destroy -Wl,--wrap=ioctl -Wl,--wrap=close -o $@ tests/capture-teardown-test.c src/ax_snapshot.c src/ax_tdp.c src/ax_rate.c src/ax_edid.c src/ax_no_signal.c src/ax_display.c $(TRANSFORM) -lm $(JPEG_LIBS)

build/snapshot-conversion-test: tests/snapshot-conversion-test.c src/ax_snapshot.c src/ax_snapshot.h | build
	$(CC) $(CFLAGS) -o $@ tests/snapshot-conversion-test.c src/ax_snapshot.c -lm $(JPEG_LIBS)

build/rate-control-test: tests/rate-control-test.c src/ax_rate.c src/ax_rate.h $(wildcard third_party/vcenc-ewl/*.h) | build
	$(CC) $(CFLAGS) -Isrc -o $@ tests/rate-control-test.c src/ax_rate.c -lm

build/vcenc-rc-test: tests/vcenc-rc-test.c $(wildcard third_party/vcenc-ewl/*.h) | build
	$(CC) $(CFLAGS) -o $@ $< -lm

build/edid-test: tests/edid-test.c src/ax_edid.c src/ax_edid.h | build
	$(CC) $(CFLAGS) -Wl,--wrap=open -Wl,--wrap=access -Wl,--wrap=read -Wl,--wrap=write -Wl,--wrap=close -o $@ tests/edid-test.c src/ax_edid.c

build/hevc-header-test: tests/hevc-header-test.c third_party/vcenc-ewl/vcenc_hevc_header.h third_party/vcenc-ewl/vcenc_header.h | build
	$(CC) -O2 -Wall $(INC) -o $@ tests/hevc-header-test.c

smoke: host
	./build/abi-smoke ./build/ax630c.so
	./build/input-format-test
	./build/capture-teardown-test
	./build/no-signal-test
	./build/display-test
	./build/pacing-capture-test
	./build/snapshot-test
	./build/encoder-buffer-test
	./build/snapshot-conversion-test
	./build/tdp-cache-test
	./build/display-capture-test
	./build/hevc-header-test
	./build/edid-test
	./build/rate-control-test
	./build/vcenc-rc-test

clean:
	rm -rf build
