SUMMARY = "AX630C native HDMI capture and VC8000E video backend"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
PR = "r7"
OPKGBUILDCMD = "opkg-build -Z gzip"

COMPATIBLE_MACHINE = "^onekvm-nanokvm-pro$"
PACKAGE_ARCH = "${MACHINE_ARCH}"
DEPENDS += "jpeg gstreamer1.0 gstreamer1.0-plugins-base"
inherit systemd pkgconfig
SYSTEMD_SERVICE:${PN} = "onekvm-vdec.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

# This source currently lives in the local Pro development tree. The ignored
# kas externalsrc overlay selects it; the published recipe will pin a Git
# revision once the AX630C backend becomes a standalone repository.
S = "/sources/onekvm-video-ax630c"
B = "${WORKDIR}/build"

# Import the owned kernel UAPI, including it in the task signature even when
# externalsrc disables do_fetch/do_unpack. No private duplicate header.
AXERA_EIP130_UAPI = "/sources/linux-6.18-ax620e/include/uapi/linux/axera_eip130.h"
python __anonymous() {
    # externalsrc replaces the task flag in its earlier anonymous setup.
    paths = [d.getVar('AXERA_EIP130_UAPI'),
             '/sources/onekvm-generic-v4l2/src/v4l2_transform.c',
             '/sources/onekvm-generic-v4l2/include/onekvm/v4l2_transform.h']
    paths += ['/sources/onekvm-core/include/onekvm/' + name for name in
              ('video_backend_v1.h', 'vdec_backend_v1.h')]
    d.appendVarFlag('do_compile', 'file-checksums', ''.join(' %s:True' % path for path in paths))
}
do_compile() {
    install -d ${B}/include/linux
    install -m 0644 ${AXERA_EIP130_UAPI} ${B}/include/linux/axera_eip130.h
    ${CC} ${CFLAGS} ${LDFLAGS} -fPIC -fvisibility=hidden -shared -pthread \
        $(pkg-config --cflags gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0) \
        -I${B}/include -I${S}/third_party/vcenc-ewl \
        -I/sources/onekvm-core/include \
        -I/sources/onekvm-generic-v4l2/include \
        -o ${B}/ax630c.so \
        ${S}/src/ax630c_video.c ${S}/src/ax_rate.c ${S}/src/ax_edid.c ${S}/src/ax_no_signal.c ${S}/src/ax_display.c ${S}/src/ax_tdp.c ${S}/src/ax_snapshot.c \
        ${S}/src/ax_vdec.c /sources/onekvm-generic-v4l2/src/v4l2_transform.c \
        $(pkg-config --libs gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0) -lm -ljpeg
    ${CC} ${CFLAGS} ${LDFLAGS} -DAX630C_STANDALONE -pthread \
        -I${B}/include -I${S}/third_party/vcenc-ewl \
        -I/sources/onekvm-core/include \
        -I/sources/onekvm-generic-v4l2/include \
        -o ${B}/ax630c-once \
        ${S}/src/ax630c_video.c ${S}/src/ax_rate.c ${S}/src/ax_edid.c ${S}/src/ax_no_signal.c ${S}/src/ax_display.c ${S}/src/ax_tdp.c ${S}/src/ax_snapshot.c \
        /sources/onekvm-generic-v4l2/src/v4l2_transform.c -lm -ljpeg
    ${CC} ${CFLAGS} ${LDFLAGS} -I${B}/include \
        -o ${B}/ax630c-eip130-ioctl-test ${S}/tests/eip130-ioctl-test.c -pthread
    ${CC} ${CFLAGS} ${LDFLAGS} -I/sources/onekvm-generic-v4l2/include \
        -o ${B}/ax630c-tdp-test ${S}/tests/tdp-v4l2-test.c \
        /sources/onekvm-generic-v4l2/src/v4l2_transform.c
    ${CC} ${CFLAGS} ${LDFLAGS} -o ${B}/ax630c-snapshot-conversion-test \
        ${S}/tests/snapshot-conversion-test.c ${S}/src/ax_snapshot.c -lm -ljpeg
    ${CC} ${CFLAGS} ${LDFLAGS} -I${B}/include -I/sources/onekvm-core/include \
        -o ${B}/ax630c-vdec-test ${S}/tests/vdec-abi-test.c -ldl -pthread
}

do_install() {
    install -Dm0644 ${S}/packaging/20-ax630c-software-srtp.conf \
        ${D}${systemd_system_unitdir}/onekvm.service.d/20-ax630c-software-srtp.conf
    install -Dm0755 ${S}/packaging/activate-vdec ${D}${libexecdir}/onekvm/activate-vdec
    install -Dm0644 ${S}/packaging/onekvm-vdec.service ${D}${systemd_system_unitdir}/onekvm-vdec.service
    install -Dm0644 ${S}/packaging/70-onekvm-vdec.rules ${D}${nonarch_base_libdir}/udev/rules.d/70-onekvm-vdec.rules
    install -d ${D}${libdir}/onekvm/video-backends
    install -m 0755 ${B}/ax630c.so ${D}${libdir}/onekvm/video-backends/ax630c.so
    install -d ${D}${datadir}/onekvm/device-backends
    install -m 0644 ${S}/packaging/ax630c-device-backend.json \
        ${D}${datadir}/onekvm/device-backends/ax630c.json
    install -d ${D}${libexecdir}/onekvm
    install -m 0755 ${B}/ax630c-once ${D}${libexecdir}/onekvm/ax630c-once
    install -m 0755 ${B}/ax630c-eip130-ioctl-test ${D}${libexecdir}/onekvm/ax630c-eip130-ioctl-test
    install -m 0755 ${B}/ax630c-tdp-test ${D}${libexecdir}/onekvm/ax630c-tdp-test
    install -m 0755 ${B}/ax630c-snapshot-conversion-test ${D}${libexecdir}/onekvm/ax630c-snapshot-conversion-test
    install -m 0755 ${B}/ax630c-vdec-test ${D}${libexecdir}/onekvm/ax630c-vdec-test
}

SOLIBS = ".so"
FILES_SOLIBSDEV = ""
FILES:${PN} = "${systemd_system_unitdir}/onekvm.service.d/20-ax630c-software-srtp.conf ${libdir}/onekvm/video-backends/ax630c.so ${datadir}/onekvm/device-backends/ax630c.json ${libexecdir}/onekvm/activate-vdec ${systemd_system_unitdir}/onekvm-vdec.service ${nonarch_base_libdir}/udev/rules.d/70-onekvm-vdec.rules"
PACKAGES =+ "${PN}-diag"
FILES:${PN}-diag = "${libexecdir}/onekvm/ax630c-once ${libexecdir}/onekvm/ax630c-eip130-ioctl-test ${libexecdir}/onekvm/ax630c-tdp-test ${libexecdir}/onekvm/ax630c-vdec-test ${libexecdir}/onekvm/ax630c-snapshot-conversion-test"
INSANE_SKIP:${PN} += "dev-so"
RPROVIDES:${PN} += "onekvm-video-backend"
RDEPENDS:${PN} += "kernel-module-ax630c-tdp"
RDEPENDS:${PN} += "kernel-module-hantro-vpu gstreamer1.0-plugins-base-app gstreamer1.0-plugins-bad-v4l2codecs gstreamer1.0-plugins-bad-videoparsersbad"
