SUMMARY = "AX630C native HDMI capture and VC8000E video backend"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
PR = "r3"
OPKGBUILDCMD = "opkg-build -Z gzip"

COMPATIBLE_MACHINE = "^onekvm-nanokvm-pro$"
PACKAGE_ARCH = "${MACHINE_ARCH}"

# This source currently lives in the local Pro development tree. The ignored
# kas externalsrc overlay selects it; the published recipe will pin a Git
# revision once the AX630C backend becomes a standalone repository.
S = "/sources/onekvm-video-ax630c"
B = "${WORKDIR}/build"

do_compile() {
    install -d ${B}
    ${CC} ${CFLAGS} ${LDFLAGS} -fPIC -fvisibility=hidden -shared -pthread \
        -I${S}/third_party/vcenc-ewl \
        -I/sources/onekvm-generic-v4l2/include \
        -o ${B}/ax630c.so \
        ${S}/src/ax630c_video.c ${S}/src/ax_no_signal.c ${S}/src/ax_display.c
    ${CC} ${CFLAGS} ${LDFLAGS} -DAX630C_STANDALONE -pthread \
        -I${S}/third_party/vcenc-ewl \
        -I/sources/onekvm-generic-v4l2/include \
        -o ${B}/ax630c-once \
        ${S}/src/ax630c_video.c ${S}/src/ax_no_signal.c ${S}/src/ax_display.c
}

do_install() {
    install -d ${D}${libdir}/onekvm/video-backends
    install -m 0755 ${B}/ax630c.so ${D}${libdir}/onekvm/video-backends/ax630c.so
    install -d ${D}${datadir}/onekvm/device-backends
    install -m 0644 ${S}/packaging/ax630c-device-backend.json \
        ${D}${datadir}/onekvm/device-backends/ax630c.json
    install -d ${D}${libexecdir}/onekvm
    install -m 0755 ${B}/ax630c-once ${D}${libexecdir}/onekvm/ax630c-once
}

SOLIBS = ".so"
FILES_SOLIBSDEV = ""
FILES:${PN} = "${libdir}/onekvm/video-backends/ax630c.so ${datadir}/onekvm/device-backends/ax630c.json"
PACKAGES =+ "${PN}-diag"
FILES:${PN}-diag = "${libexecdir}/onekvm/ax630c-once"
INSANE_SKIP:${PN} += "dev-so"
RPROVIDES:${PN} += "onekvm-video-backend"
