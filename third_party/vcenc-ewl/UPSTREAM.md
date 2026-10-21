# VC8000E headers

The headers in this directory come from the `pkgs/vcenc-ewl` implementation in
[GoogleBot42/open-nanokvm-pro](https://github.com/GoogleBot42/open-nanokvm-pro).
Their SPDX identifiers offer `GPL-2.0 OR MIT`; OneKVM uses the MIT option.

`vcenc_hevc_header.h` and `tests/hevc-header-test.c` were imported from commit
`e7657409589ad3cd2cb8623849c4682e4257f83b` (upstream test path
`pkgs/vcenc-ewl/tests/hevc_header_test.c`; only its include path was adapted).
The local VUI colour fields signal limited-range BT.709, matching captured
HDMI YUYV and H.264. The upstream full-range BT.601 declaration did not match
these pixels. The header tests reflect this intentional change; all other
syntax matches the existing `ENC_CODEC_HEVC` register program and fixed-QP
mode, including the observed CTU/RPS/POC layout.

`vcenc_rc.h` and `tests/vcenc-rc-test.c` come from commit
`6de5a562c048600b4a61ffefc223ef5aaffbf915`; test include paths are adapted.
The frame controller selects QP inside the configured bounds and feeds the
fixed-QP hardware program. PPS QP stays in `pic_init_qp`; the frame QP may differ.
