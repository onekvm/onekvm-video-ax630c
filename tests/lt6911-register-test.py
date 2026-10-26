#!/usr/bin/env python3
"""Exercise the actual family driver's register/flash functions with mock I2C.

This is a native host test, not a replacement for bridge hardware validation.
No copy of the driver's logic is maintained here: extract its C functions.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
driver = root / 'linux-6.18-ax620e/drivers/misc/lt6911.c'
source = driver.read_text()
names = '''lt6911_write lt6911_read lt6911_read_be16 lt6911_write_block
lt6911_read_block lt6911_write_sys_seq lt6911_enable lt6911_disable
lt6911_disable_watchdog lt6911_chip_name lt6911_check_chip_id
lt6911_get_rx_geometry lt6911_get_signal_state lt6911_get_res lt6911_get_fps
lt6911d_seq lt6911d_flash_begin lt6911d_flash_end lt6911d_flash_reset_fifo
lt6911d_flash_addr lt6911d_flash_read lt6911d_flash_erase lt6911d_flash_write_page
lt6911d_write_metadata lt6911d_edid_write lt6911_flash_ack
lt6911_flash_write_page lt6911_flash_read_page lt6911_version_read
lt6911_version_write lt6911_edid_read lt6911_edid_write
lt6911_video_lost lt6911_init_text'''.split()
parts = []
for name in names:
    match = re.search(r'^static [^{;]*\b' + name + r'\([^;]*?\n\{', source, re.M)
    assert match, name
    end = source.index('\n}\n', match.end()) + 3
    parts.append((match.start(), source[match.start():end]))
for pattern in [r'enum lt6911_chip', r'enum lt6911_video',
                r'struct lt6911_regval', r'struct lt6911d_step']:
    start = re.search(r'^' + pattern + r' \{', source, re.M).start()
    end = source.index('\n};', start) + 3
    parts.append((start, source[start:end]))
for name in ['lt6911_flash_erase_seq', 'lt6911_flash_prog_seq', 'lt6911_flash_read_seq']:
    start = source.index('static const struct lt6911_regval ' + name)
    end = source.index('\n};', start) + 3
    parts.append((start, source[start:end]))
defines = '\n'.join(line for line in source.splitlines()
                    if line.startswith('#define LT6911') and not line.endswith('\\'))

prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define U16_MAX UINT16_MAX
#define div64_u64(n, d) ((u64)(n) / (u64)(d))
#define dev_err(...) ((void)0)
#define dev_info(...) ((void)0)
#define dev_dbg(...) ((void)0)
static void msleep(unsigned n) { (void)n; }
static void usleep_range(unsigned lo, unsigned hi) { (void)lo; (void)hi; }
#include <stdarg.h>
struct mock_text { char value[64]; };
struct lt6911 {
    void *client, *dev; int chip, buf_lock;
    struct mock_text status, width, height, fps, hdcp, asr;
};
static void mutex_lock(int *p) { (void)p; }
static void mutex_unlock(int *p) { (void)p; }
static void lt6911_bump_status(struct lt6911 *lt) { (void)lt; }
static void lt6911_set_text(struct mock_text *text, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text->value, sizeof(text->value), fmt, ap);
    va_end(ap);
}
static u8 regs[256][256], flash[0x20000], bank, fifo[32];
static unsigned fifo_len, calls, fail_at, erases, programs, short_block;
static int bus_fault(void) { return ++calls == fail_at ? -EIO : 0; }
static unsigned address(void) {
    return ((unsigned)regs[bank][0x5b] << 16) |
           ((unsigned)regs[bank][0x5c] << 8) | regs[bank][0x5d];
}
static int i2c_smbus_write_byte_data(void *client, u8 reg, u8 value) {
    (void)client;
    if (bus_fault()) return -EIO;
    if (reg == 0xff) { bank = value; return 0; }
    regs[bank][reg] = value;
    if (bank == 0xe1 && reg == 3 && value == 0xbf) fifo_len = 0;
    if (bank == 0xe0 && reg == 0x5a && value == 1) {
        unsigned addr = address() & ~0xfffU;
        assert(addr == 0x8000);
        memset(flash + addr, 0xff, 4096);
        erases++;
    }
    if (bank == 0xe0 && reg == 0x5a && value == 0x10 && fifo_len) {
        unsigned addr = address();
        assert(addr >= 0x8000 && addr + fifo_len <= 0x8120);
        memcpy(flash + addr, fifo, fifo_len);
        fifo_len = 0;
        programs++;
    }
    return 0;
}
static int i2c_smbus_read_byte_data(void *client, u8 reg) {
    (void)client;
    if (bus_fault()) return -EIO;
    return regs[bank][reg];
}
static int i2c_smbus_write_i2c_block_data(void *client, u8 reg, u8 len, const u8 *data) {
    (void)client;
    if (bus_fault()) return -EIO;
    assert(bank == 0xe0 && reg == 0x59 && len <= sizeof(fifo));
    memcpy(fifo, data, len);
    fifo_len = len;
    return 0;
}
static int i2c_smbus_read_i2c_block_data(void *client, u8 reg, u8 len, u8 *data) {
    (void)client;
    if (bus_fault()) return -EIO;
    assert(bank == 0xe0 && reg == 0x5f);
    assert(address() + len <= sizeof(flash));
    memcpy(data, flash + address(), len);
    return short_block ? len - 1 : len;
}
'''

tests = r'''
static void reset(void) {
    memset(regs, 0, sizeof(regs));
    memset(flash, 0xff, sizeof(flash));
    bank = fifo_len = calls = fail_at = erases = programs = short_block = 0;
}
static void be16(u8 b, u8 r, unsigned v) { regs[b][r] = v >> 8; regs[b][r+1] = v; }
static void clock20(u8 b, u8 r, unsigned v) {
    regs[b][r] = v >> 16; regs[b][r+1] = v >> 8; regs[b][r+2] = v;
}
int main(void) {
    struct lt6911 lt = {0};
    lt6911_init_text(&lt);
    assert(!strcmp(lt.hdcp.value, "unknown hdcp\n"));
    for (int chip = LT6911_CHIP_UXC; chip <= LT6911_CHIP_D; chip++) {
        lt.chip = chip;
        lt6911_video_lost(&lt, true);
        assert(!strcmp(lt.hdcp.value, chip == LT6911_CHIP_UXC ?
                       "no hdcp\n" : "unknown hdcp\n"));
        lt6911_video_lost(&lt, false);
        assert(!strcmp(lt.hdcp.value, "unknown hdcp\n"));
    }
    u16 w, h, fps;
    enum lt6911_video video;
    bool audio;
    u8 edid[256], readback[256], version[32], saved_flash[sizeof(flash)];
    char text[33];
    for (unsigned i = 0; i < sizeof(edid); i++) edid[i] = i ^ 0x5a;
    memset(version, 0xff, sizeof(version));
    memcpy(version, "NanoKVM_Pro (Desk-H) NebF10022", sizeof("NanoKVM_Pro (Desk-H) NebF10022") - 1);

    reset(); lt.chip = LT6911_CHIP_UNKNOWN;
    assert(lt6911_check_chip_id(&lt) == -ENODEV);
    assert(lt.chip == LT6911_CHIP_UNKNOWN && !erases && !programs);
    reset(); regs[0x81][0] = 0x17; regs[0x81][1] = 4; regs[0x81][2] = 0x83;
    assert(!lt6911_check_chip_id(&lt) && lt.chip == LT6911_CHIP_UXC);
    be16(0x85, 0xea, 1920); be16(0x85, 0xf0, 1080);
    assert(!lt6911_get_res(&lt, &w, &h) && w == 1920 && h == 1080);
    be16(0xd4, 0x26, 2200); be16(0xd4, 0x32, 1125); clock20(0x85, 0x48, 74250);
    assert(!lt6911_get_fps(&lt, &fps) && fps == 60);
    regs[0x86][0xa3] = 0x55; regs[0xb0][0x81] = 0x20;
    assert(!lt6911_get_signal_state(&lt, &video, &audio) && video == LT6911_VIDEO_STABLE && audio);

    reset(); lt.chip = LT6911_CHIP_UNKNOWN; regs[0xa0][0] = 0x16; regs[0xa0][1] = 5;
    assert(!lt6911_check_chip_id(&lt) && lt.chip == LT6911_CHIP_C);
    be16(0xc2, 0x38, 1920); be16(0xc2, 0x06, 1080);
    assert(!lt6911_get_res(&lt, &w, &h) && w == 1920 && h == 1080);
    be16(0xd2, 0x8b, 1920); be16(0xd2, 0x96, 1080); regs[0xd1][0x55] = 48;
    assert(!lt6911_get_signal_state(&lt, &video, &audio) && video == LT6911_VIDEO_STABLE && audio);
    unsigned before = calls;
    assert(!lt6911_get_fps(&lt, &fps) && fps == 0 && calls == before);
    assert(lt6911_edid_read(&lt, readback) == -EOPNOTSUPP);
    assert(lt6911_edid_write(&lt, edid) == -EOPNOTSUPP);
    assert(lt6911_version_write(&lt, version, 32) == -EOPNOTSUPP);
    assert(calls == before && !erases && !programs);

    reset(); lt.chip = LT6911_CHIP_UNKNOWN;
    memcpy(regs[0xe0] + 0x80, (u8[]){0x25,0x10,0x23,1}, 4);
    assert(!lt6911_check_chip_id(&lt) && lt.chip == LT6911_CHIP_D);
    be16(0xe0, 0x8c, 960); be16(0xe0, 0x8e, 1080); be16(0xe0, 0x90, 48);
    assert(!lt6911_get_res(&lt, &w, &h) && w == 1920 && h == 1080);
    assert(!lt6911_get_signal_state(&lt, &video, &audio) && video == LT6911_VIDEO_STABLE && audio);
    be16(0xe0, 0x8e, 0);
    assert(!lt6911_get_signal_state(&lt, &video, &audio) && video == LT6911_VIDEO_GONE && !audio);
    be16(0xe0, 0x88, 1100); be16(0xe0, 0x8a, 1125); clock20(0xe0, 0x85, 74250);
    assert(!lt6911_get_fps(&lt, &fps) && fps == 60);
    be16(0xe0, 0x88, 2200); be16(0xe0, 0x8a, 2250); clock20(0xe0, 0x85, 148500);
    assert(!lt6911_get_fps(&lt, &fps) && fps == 30);
    be16(0xe0, 0x88, 0);
    assert(!lt6911_get_fps(&lt, &fps) && fps == 0);
    be16(0xe0, 0x8c, 40000);
    assert(lt6911_get_rx_geometry(&lt, &w, &h) == -ERANGE);

    reset(); lt.chip = LT6911_CHIP_D;
    memcpy(flash + 0x8100, version, sizeof(version));
    assert(!lt6911_edid_write(&lt, edid));
    assert(erases == 1 && programs == 9);
    assert(!memcmp(flash + 0x8000, edid, sizeof(edid)));
    assert(!memcmp(flash + 0x8100, version, sizeof(version)));
    assert(!lt6911_edid_read(&lt, readback) && !memcmp(readback, edid, sizeof(edid)));
    assert(!lt6911_version_read(&lt, text) && !strcmp(text, "NanoKVM_Pro (Desk-H) NebF10022"));
    assert(lt6911_version_write(&lt, version, sizeof(version)) == -EIO && erases == 1);
    memset(flash + 0x8100, 0xff, 32);
    assert(!lt6911_version_write(&lt, version, sizeof(version)));
    assert(!memcmp(flash + 0x8000, edid, 256));
    short_block = 1;
    assert(lt6911_edid_read(&lt, readback) == -EIO);
    short_block = 0;

    /* Every failed transaction in the read path must return an error,
     * never erase flash, and always release the flash interface. */
    calls = 0;
    assert(!lt6911_edid_read(&lt, readback));
    unsigned read_calls = calls;
    memcpy(saved_flash, flash, sizeof(flash));
    for (unsigned n = 1; n <= read_calls; n++) {
        calls = 0; fail_at = n;
        assert(lt6911_edid_read(&lt, readback) < 0);
        assert(!memcmp(flash, saved_flash, sizeof(flash)));
    }
    printf("LT6911 UXC/C/D register tests passed (%u read failure points)\n", read_calls);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='lt6911-register-') as work:
    path = Path(work)
    (path / 'test.c').write_text(prefix + '\n' + defines + '\n' +
        '\n\n'.join(text for _, text in sorted(parts)) + '\n' + tests)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-sign-compare',
        '-fsanitize=address,undefined', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
