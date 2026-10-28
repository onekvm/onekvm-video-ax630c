#!/usr/bin/env python3
"""Exercise actual LT6911 power/proc functions with a slow-read hardware model."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
driver = Path(os.environ.get('ONEKVM_LT6911_DRIVER',
                            root / 'linux-6.18-ax620e/drivers/misc/lt6911.c'))
source = driver.read_text()
functions = []
for name in ['lt6911_set_power', 'lt6911_power_cycle', 'lt6911_edid_proc_read']:
    match = re.search(r'^static [^{;]*\b' + name + r'\([^;]*?\n\{', source, re.M)
    assert match, name
    end = source.index('\n}\n', match.end()) + 3
    functions.append(source[match.start():end])
defines = '\n'.join(line for line in source.splitlines()
                    if line.startswith(('#define LT6911_EDID_SIZE\t',
                                        '#define LT6911_EDID_READ_INTERVAL_MS\t')))
assert 'LT6911_EDID_READ_INTERVAL_MS' in defines

model = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
typedef uint8_t u8;
typedef int64_t s64;
typedef int64_t ktime_t;
#define __user
#define dev_err(...) ((void)0)
struct text { char value[8]; };
struct lt6911 {
    void *pwr_gpio, *dev;
    int chip_lock, buf_lock;
    bool powered, shadow_override, shadow_restore_pending;
    struct text power;
    u8 shadow_desired[256], edid_snapshot[256], edid[256];
    size_t edid_snapshot_len, edid_len;
    ktime_t edid_read_ts;
};
struct file { struct lt6911 *lt; };
#define file_inode(file) (file)
#define pde_data(inode) ((inode)->lt)
static ktime_t clock_ms = 1000, ready_at;
static bool rail_on;
static unsigned reads, gpio_calls;
static int gpio_error, read_error;
static u8 flash[256];
static void msleep(unsigned ms) { clock_ms += ms; }
static ktime_t ktime_get(void) { return clock_ms; }
static ktime_t ktime_sub(ktime_t a, ktime_t b) { return a - b; }
static s64 ktime_to_ms(ktime_t t) { return t; }
static void mutex_lock(int *p) { (void)p; }
static void mutex_unlock(int *p) { (void)p; }
static int gpiod_set_value_cansleep(void *gpio, bool on) {
    (void)gpio;
    ++gpio_calls;
    if (gpio_error) return gpio_error;
    if (on && !rail_on) ready_at = clock_ms + 300;
    rail_on = on;
    return 0;
}
static void lt6911_set_text(struct text *text, const char *value) {
    snprintf(text->value, sizeof(text->value), "%s", value);
}
static int lt6911uxc_shadow_apply(struct lt6911 *lt, const u8 *desired, u8 *actual) {
    (void)lt;
    assert(rail_on && clock_ms >= ready_at);
    memcpy(actual, desired, 256);
    return 0;
}
static int lt6911_edid_read(struct lt6911 *lt, u8 *data) {
    (void)lt;
    ++reads;
    if (read_error) return read_error;
    if (!rail_on || clock_ms < ready_at) return -EREMOTEIO;
    /* The real D bridge takes ~550 ms at its 100 kHz I2C clock. */
    clock_ms += 550;
    memcpy(data, flash, 256);
    return 0;
}
static ssize_t simple_read_from_buffer(char *to, size_t count, loff_t *pos,
                                       const void *from, size_t length) {
    if (*pos < 0) return -EINVAL;
    if (!count || (size_t)*pos >= length) return 0;
    if (count > length - (size_t)*pos) count = length - (size_t)*pos;
    memcpy(to, (const char *)from + *pos, count);
    *pos += count;
    return (ssize_t)count;
}
'''
tests = r'''
int main(void) {
    struct lt6911 lt = {0};
    struct file file = { .lt = &lt };
    char data[256];
    loff_t pos = 256;
    for (unsigned i = 0; i < 256; ++i) flash[i] = i ^ 0x5a;
    /* EOF and zero-length reads must not even power on the bridge. */
    assert(!lt6911_edid_proc_read(&file, data, sizeof(data), &pos));
    pos = 0;
    assert(!lt6911_edid_proc_read(&file, data, 0, &pos));
    pos = -1;
    assert(lt6911_edid_proc_read(&file, data, sizeof(data), &pos) == -EINVAL);
    assert(!reads && !gpio_calls && !rail_on);

    /* Implicit wake must settle just like an explicit /proc/power wake. */
    pos = 0;
    assert(lt6911_edid_proc_read(&file, data, sizeof(data), &pos) == 256);
    assert(pos == 256 && reads == 1 && gpio_calls == 3 && rail_on);
    assert(clock_ms >= ready_at && !memcmp(data, flash, 256));
    assert(lt.edid_snapshot_len == 256 && !memcmp(lt.edid_snapshot, flash, 256));
    assert(lt.edid_read_ts == clock_ms);
    unsigned saved_gpio = gpio_calls, saved_reads = reads;

    /* cat's EOF syscall does not repeat the slow read/reset transaction. */
    assert(!lt6911_edid_proc_read(&file, data, sizeof(data), &pos));
    clock_ms += 1000;
    assert(!lt6911_edid_proc_read(&file, data, sizeof(data), &pos));
    assert(reads == saved_reads && gpio_calls == saved_gpio);

    /* A new open refreshes an expired cache once. Its immediate successor
       uses the completed refresh timestamp, not the pre-I2C start time. */
    pos = 0;
    assert(lt6911_edid_proc_read(&file, data, sizeof(data), &pos) == 256);
    assert(reads == saved_reads + 1 && lt.edid_read_ts == clock_ms);
    saved_gpio = gpio_calls; saved_reads = reads;
    pos = 0;
    assert(lt6911_edid_proc_read(&file, data, sizeof(data), &pos) == 256);
    assert(reads == saved_reads && gpio_calls == saved_gpio);
    assert(!memcmp(data, flash, 256));

    pos = 0;
    assert(lt6911_edid_proc_read(&file, data, 16, &pos) == 16 && pos == 16);
    assert(lt6911_edid_proc_read(&file, data + 16, 240, &pos) == 240 && pos == 256);
    assert(!memcmp(data, flash, 256));
    assert(reads == saved_reads && gpio_calls == saved_gpio);

    /* Genuine GPIO/I2C failures remain errors and cannot freshen the cache. */
    assert(!lt6911_set_power(&lt, false));
    clock_ms += 1000;
    ktime_t timestamp = lt.edid_read_ts;
    pos = 0; gpio_error = -EIO;
    assert(lt6911_edid_proc_read(&file, data, sizeof(data), &pos) == -EIO);
    assert(!pos && !lt.powered && reads == saved_reads && lt.edid_read_ts == timestamp);
    gpio_error = 0; read_error = -EREMOTEIO;
    assert(lt6911_edid_proc_read(&file, data, sizeof(data), &pos) == -EREMOTEIO);
    assert(!pos && lt.edid_read_ts == timestamp);
    read_error = 0;
    assert(lt6911_edid_proc_read(&file, data, sizeof(data), &pos) == 256);
    assert(lt.edid_read_ts == clock_ms && !memcmp(data, flash, 256));
    printf("LT6911 proc: cold wake, slow-read cache, EOF, short reads and failure recovery passed\n");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='lt6911-proc-') as work:
    path = Path(work)
    (path / 'test.c').write_text(defines + '\n' + model + '\n' +
                               '\n\n'.join(functions) + '\n' + tests)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', str(path / 'test.c'), '-o', str(path / 'test')],
        check=True)
    subprocess.run([str(path / 'test')], check=True)
