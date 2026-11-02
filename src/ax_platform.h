/* SPDX-License-Identifier: MIT */
#ifndef AX_PLATFORM_H
#define AX_PLATFORM_H
/* The encoder and DMA ABI are shared; board wiring and bridge protocols are not. */
#ifdef AXERA_NANOKVM_GO
#define AX_BRIDGE_PROC "/proc/lt7911_info"
#define AX_BOARD_ID "go"
#define AX_DRIVER_ID "ax620q"
#define AX_CAPTURE_BUFFERS 4
#define AX_MAX_INPUT_WIDTH 1920u
#define AX_MAX_WIDTH 1920u
#define AX_MAX_HEIGHT 1080u
#else
#define AX_BRIDGE_PROC "/proc/lt6911_info"
#define AX_BOARD_ID "pro"
#define AX_DRIVER_ID "ax630c"
#define AX_CAPTURE_BUFFERS 8
#define AX_MAX_INPUT_WIDTH 3840u
#define AX_MAX_WIDTH 4096u
#define AX_MAX_HEIGHT 2400u
#endif
#endif
