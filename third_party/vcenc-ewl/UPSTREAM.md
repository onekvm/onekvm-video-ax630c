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
