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
lt6911c_flash_end lt6911c_flash_address lt6911c_flash_read lt6911c_flash_ack
lt6911c_flash_prepare lt6911c_flash_erase lt6911c_flash_write_page
lt6911c_edid_read lt6911c_edid_write lt6911_check_edid
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
#include <stdlib.h>
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
static unsigned c_reads, unstable_read, erase_size = 4096, bad_program;
static int alloc_failure;
static unsigned erase_call, program_call;
#define GFP_KERNEL 0
static void *kvmalloc(size_t size, int flags) {
    (void)flags;
    return alloc_failure ? NULL : malloc(size);
}
static void kvfree(void *p) { free(p); }
static void *memchr_inv(const void *p, int c, size_t n) {
    const u8 *bytes = p;
    for (size_t i = 0; i < n; i++)
        if (bytes[i] != (u8)c) return (void *)(bytes + i);
    return NULL;
}
static int lt6911_check_edid(struct lt6911 *lt, const u8 *edid, size_t len);
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
    if (bank == 0x80 && reg == 0x5a && value == 0x86) fifo_len = 0;
    if (bank == 0x80 && reg == 0x5a && value == 0x83) {
        unsigned addr = address();
        assert(addr == 0x18000);
        erase_call = calls;
        addr &= ~(erase_size - 1);
        assert(addr >= 0x10000 && addr + erase_size <= sizeof(flash));
        memset(flash + addr, 0xff, erase_size);
        erases++;
    }
    if (bank == 0x80 && reg == 0x5a && value == 0x92 &&
        regs[bank][0x5e] == 0xe0 && fifo_len) {
        unsigned addr = address();
        assert(addr >= 0x18000 && addr + fifo_len <= sizeof(flash));
        for (unsigned i = 0; i < fifo_len; i++) flash[addr + i] &= fifo[i];
        if (bad_program) flash[addr] ^= 1;
        program_call = calls;
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
    assert((bank == 0xe0 || bank == 0x80) && reg == 0x59 && len <= sizeof(fifo));
    memcpy(fifo, data, len);
    fifo_len = len;
    return 0;
}
static int i2c_smbus_read_i2c_block_data(void *client, u8 reg, u8 len, u8 *data) {
    (void)client;
    if (bus_fault()) return -EIO;
    assert((bank == 0xe0 || bank == 0x80) && reg == 0x5f);
    assert(address() + len <= sizeof(flash));
    memcpy(data, flash + address(), len);
    if (bank == 0x80 && ++c_reads == unstable_read) data[0] ^= 1;
    return short_block ? len - 1 : len;
}
'''

tests = r'''
static void reset(void) {
    memset(regs, 0, sizeof(regs));
    memset(flash, 0xff, sizeof(flash));
    bank = fifo_len = calls = fail_at = erases = programs = short_block = 0;
    c_reads = unstable_read = bad_program = alloc_failure = 0;
    erase_call = program_call = 0;
    regs[0x90][2] = 0xff;
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
    /* Valid C fixture with two blocks, and a distinct valid replacement. */
    u8 original[256] = {0,255,255,255,255,255,255,0};
    original[126] = 1; original[127] = 5; /* header sums to 250 */
    original[128] = 2; original[255] = 254;
    assert(!lt6911_check_edid(&lt, original, 256));
    memcpy(edid, original, sizeof(edid));
    edid[12] = 42; edid[127] -= 42;
    lt.chip = LT6911_CHIP_C;
    for (erase_size = 4096; erase_size <= 65536; erase_size *= 16) {
        reset(); regs[0xa0][0] = 0x16; regs[0xa0][1] = 5;
        /* Non-EDID bytes in the bank must survive both erase variants. */
        memset(flash, 0x53, 0x10000);
        memcpy(flash + 0x18000, original, sizeof(original));
        flash[0x18100] = 0x91; flash[0x18fff] = 0xa2; flash[0x1fff0] = 0xb3;
        memcpy(saved_flash, flash, sizeof(flash));
        assert(!lt6911_edid_read(&lt, readback) && !memcmp(original, readback, 256));
        assert(!lt6911_edid_write(&lt, edid));
        assert(erases == 1 && programs == 19);
        memcpy(saved_flash + 0x18000, edid, sizeof(edid));
        assert(!memcmp(flash, saved_flash, sizeof(flash)));
        assert(regs[0x80][0xee] == 0 && regs[0x80][0x5a] == 0x82);
        assert(!lt6911_edid_write(&lt, edid) && erases == 1 && programs == 19);
    }
    erase_size = 4096;
    reset(); regs[0xa0][0] = 0x16; regs[0xa0][1] = 5;
    assert(!lt6911_edid_read(&lt, readback));
    assert(!memchr_inv(readback, 0xff, sizeof(readback)));
    assert(!lt6911_edid_write(&lt, edid) && erases == 1);
    reset(); regs[0xa0][0] = 0x16; regs[0xa0][1] = 5;
    flash[0x18000] = 0;
    assert(lt6911_edid_read(&lt, readback) == -EOPNOTSUPP);
    assert(lt6911_edid_write(&lt, edid) == -EOPNOTSUPP && !erases);
    memcpy(flash + 0x18000, original, sizeof(original));
    flash[0x10000] = 0;
    assert(lt6911_edid_write(&lt, edid) == -EOPNOTSUPP && !erases);
    flash[0x10000] = 0xff;
    unstable_read = 4097; c_reads = 0;
    assert(lt6911_edid_write(&lt, edid) == -EIO && !erases);
    unstable_read = 0;
    edid[127]++;
    before = calls;
    assert(lt6911_edid_write(&lt, edid) == -EINVAL && calls == before);
    edid[127]--;
    alloc_failure = 1;
    assert(lt6911_edid_write(&lt, edid) == -ENOMEM && calls == before);
    alloc_failure = 0;
    regs[0xa0][0] = 0x17;
    assert(lt6911_edid_write(&lt, edid) == -ENODEV && !erases && !programs);
    regs[0xa0][0] = 0x16;
    regs[0x90][2] = 0;
    assert(lt6911_edid_write(&lt, edid) == -EIO && !erases && !programs);
    regs[0x90][2] = 0xff;
    short_block = 1;
    assert(lt6911_edid_read(&lt, readback) == -EIO);
    assert(lt6911_edid_write(&lt, edid) == -EIO && !erases && !programs);
    short_block = 0;
    before = calls;
    assert(lt6911_version_write(&lt, version, 32) == -EOPNOTSUPP && calls == before);
    assert(lt6911c_flash_read(&lt, 0, readback, 256) == -EINVAL);
    assert(lt6911c_flash_read(&lt, 0x1fff0, readback, 32) == -EINVAL);
    assert(lt6911c_flash_read(&lt, 0x18001, readback, 16) == -EINVAL);
    assert(lt6911c_flash_read(&lt, 0x18000, readback, 0) == -EINVAL);
    assert(lt6911c_flash_write_page(&lt, 0x17ff0, edid) == -EINVAL);
    assert(lt6911c_flash_write_page(&lt, 0x20000, edid) == -EINVAL);
    assert(calls == before);

    calls = 0;
    assert(!lt6911_edid_read(&lt, readback));
    unsigned c_read_calls = calls;
    memcpy(saved_flash, flash, sizeof(flash));
    for (unsigned n = 1; n <= c_read_calls; n++) {
        calls = 0; fail_at = n;
        assert(lt6911_edid_read(&lt, readback) < 0);
        assert(!memcmp(flash, saved_flash, sizeof(flash)) && !erases && !programs);
        /* A failed cleanup transaction cannot promise that register's value. */
        if (n < c_read_calls - 1) assert(regs[0x80][0xee] == 0);
    }
    fail_at = 0;
    /* Exercise every transfer in the destructive primitives separately from
     * the long double-read prefix (already covered by the read tests). */
    calls = 0;
    assert(!lt6911c_flash_prepare(&lt));
    unsigned prepare_calls = calls;
    for (unsigned n = 1; n <= prepare_calls; n++) {
        regs[0x90][2] = 0xff;
        calls = 0; fail_at = n;
        assert(lt6911c_flash_prepare(&lt) < 0 && !erases && !programs);
        fail_at = 0; assert(!lt6911c_flash_end(&lt));
    }
    regs[0x90][2] = 0xff;
    calls = 0;
    assert(!lt6911c_flash_erase(&lt));
    unsigned erase_calls = calls;
    for (unsigned n = 1; n <= erase_calls; n++) {
        regs[0x90][2] = 0xff;
        calls = 0; fail_at = n;
        assert(lt6911c_flash_erase(&lt) < 0);
        fail_at = 0; assert(!lt6911c_flash_end(&lt));
    }
    calls = 0;
    assert(!lt6911c_flash_write_page(&lt, 0x18000, edid));
    unsigned page_calls = calls;
    for (unsigned n = 1; n <= page_calls; n++) {
        calls = 0; fail_at = n;
        assert(lt6911c_flash_write_page(&lt, 0x18000, edid) < 0);
        fail_at = 0; assert(!lt6911c_flash_end(&lt));
    }
    reset(); regs[0xa0][0] = 0x16; regs[0xa0][1] = 5;
    memcpy(flash + 0x18000, original, 256);
    assert(!lt6911_edid_write(&lt, edid));
    unsigned write_calls = calls, erase_at = erase_call, last_program_at = program_call;
    unsigned write_failures = 0;
    /* Whole writer: every transfer from chip revalidation through final ACK,
     * then the verification read entry and final cleanup. The much longer
     * bank reads use the same read primitive tested above. */
    for (unsigned n = erase_at - prepare_calls - 6; n <= write_calls; n++) {
        if (n > last_program_at + 24 && n < write_calls - 4) continue;
        reset(); regs[0xa0][0] = 0x16; regs[0xa0][1] = 5;
        memcpy(flash + 0x18000, original, 256);
        memset(flash, 0x53, 0x10000);
        calls = 0; fail_at = n;
        assert(lt6911_edid_write(&lt, edid) < 0);
        for (unsigned i = 0; i < 0x10000; i++) assert(flash[i] == 0x53);
        write_failures++;
    }
    reset(); regs[0xa0][0] = 0x16; regs[0xa0][1] = 5;
    memcpy(flash + 0x18000, original, 256);
    bad_program = 1;
    assert(lt6911_edid_write(&lt, edid) == -EIO && erases == 1);
    assert(regs[0x80][0xee] == 0);
    printf("LT6911 UXC/C/D register tests passed (D %u, C %u read failure points; "
           "C prepare/erase/page %u/%u/%u, writer %u)\n", read_calls, c_read_calls,
           prepare_calls, erase_calls, page_calls, write_failures);
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
