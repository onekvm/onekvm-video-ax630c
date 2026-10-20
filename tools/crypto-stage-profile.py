#!/usr/bin/env python3
"""Differential profiling of native GCM/CTR at the same operations/second.

Use GCM with N bytes versus CTR with N+16 bytes to match the kernel GCM
cipher input. The worker CPU delta estimates GHASH/template cost. Compare
short and long CTR at the same rate to estimate buffer copy/clear cost.
This diagnostic includes Python in caller CPU; use onekvm-crypto-bench for
software/hardware throughput and total CPU comparisons.
"""
import argparse
import ctypes as c
import json
import os
from pathlib import Path
import time

class GcmRequest(c.Structure):
    _fields_ = [('size', c.c_uint32), ('flags', c.c_uint32), ('aad', c.c_uint64),
                ('src', c.c_uint64), ('dst', c.c_uint64), ('aad_size', c.c_uint32),
                ('src_size', c.c_uint32), ('capacity', c.c_uint32), ('written', c.c_uint32),
                ('nonce', c.c_ubyte * 12), ('nonce_size', c.c_uint32)]
class CtrRequest(c.Structure):
    _fields_ = [('size', c.c_uint32), ('flags', c.c_uint32), ('src', c.c_uint64),
                ('dst', c.c_uint64), ('src_size', c.c_uint32), ('capacity', c.c_uint32),
                ('written', c.c_uint32), ('iv', c.c_ubyte * 16), ('iv_size', c.c_uint32)]
CreateGcm = c.CFUNCTYPE(c.c_int32, c.c_void_p, c.c_uint32, c.c_uint32, c.POINTER(c.c_void_p), c.c_void_p, c.c_uint32)
CreateCtr = c.CFUNCTYPE(c.c_int32, c.c_uint32, c.c_void_p, c.c_uint32, c.POINTER(c.c_void_p), c.c_void_p, c.c_uint32)
Seal = c.CFUNCTYPE(c.c_int32, c.c_void_p, c.c_void_p, c.c_void_p, c.c_uint32)
Destroy = c.CFUNCTYPE(None, c.c_void_p)
class Api(c.Structure):
    _fields_ = [('size', c.c_uint32), ('abi', c.c_uint32), ('driver', c.c_char_p),
                ('name', c.c_char_p), ('features', c.c_uint64), ('batch', c.c_uint32),
                ('input_size', c.c_uint32), ('alignment', c.c_uint32), ('reserved', c.c_uint32),
                ('probe', c.c_void_p), ('create_gcm', CreateGcm), ('seal', Seal),
                ('batch_fn', c.c_void_p), ('destroy', Destroy), ('create_ctr', CreateCtr),
                ('run_ctr', Seal), ('max_primitive', c.c_uint32), ('reserved2', c.c_uint32)]

def snapshot(worker):
    values = list(map(int, Path('/proc/stat').read_text().splitlines()[0].split()[1:9]))
    stat = Path(f'/proc/{worker}/stat').read_text().rsplit(')', 1)[1].split()
    worker_ticks = int(stat[11]) + int(stat[12])
    irq = ipi = 0
    for line in Path('/proc/interrupts').read_text().splitlines():
        if '4900000.crypto' in line or line.startswith('IPI'):
            fields = line.split(':', 1)[1].split()
            count = sum(int(x) for x in fields[:2])
            if line.startswith('IPI'): ipi += count
            else: irq += count
    return {'total': sum(values), 'busy': sum(values)-values[3]-values[4],
            'worker_ticks': worker_ticks, 'irq': irq, 'ipi': ipi}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--algorithm', choices=['gcm', 'ctr'], default='gcm')
    parser.add_argument('--bytes', type=int, default=1200)
    parser.add_argument('--pps', type=float, default=2000)
    parser.add_argument('--seconds', type=float, default=8)
    parser.add_argument('--library', default='/usr/lib/onekvm/video-backends/ax630c.so')
    args = parser.parse_args()
    assert 0 < args.bytes <= 65524 and args.pps > 0 and args.seconds >= 1
    worker = next(p.name for p in Path('/proc').iterdir() if p.name.isdigit()
                  and (p/'comm').exists() and (p/'comm').read_text().strip() == '4900000.crypto-engine')
    lib = c.CDLL(args.library)
    ptr = c.c_void_p()
    assert lib.onekvm_crypto_backend_query(1, c.byref(ptr)) == 0
    api = c.cast(ptr, c.POINTER(Api)).contents
    assert api.size >= c.sizeof(Api)
    key = c.create_string_buffer(os.urandom(16), 16)
    src = c.create_string_buffer(args.bytes)
    dst = c.create_string_buffer(args.bytes+16)
    aad = c.create_string_buffer(12)
    error = c.create_string_buffer(256)
    session = c.c_void_p()
    if args.algorithm == 'gcm':
        result = api.create_gcm(key, 16, 16, c.byref(session), error, 256)
        req = GcmRequest(size=c.sizeof(GcmRequest), src=c.addressof(src), dst=c.addressof(dst),
                         src_size=args.bytes, capacity=args.bytes+16, aad=c.addressof(aad), aad_size=12, nonce_size=12)
        call = api.seal
    else:
        result = api.create_ctr(1, key, 16, c.byref(session), error, 256)
        req = CtrRequest(size=c.sizeof(CtrRequest), src=c.addressof(src), dst=c.addressof(dst),
                         src_size=args.bytes, capacity=args.bytes+16, iv_size=16)
        call = api.run_ctr
    assert result == 0, error.value
    counter = 0
    def run():
        nonlocal counter
        counter += 1
        nonce = counter.to_bytes(8, 'big')
        array = req.nonce if args.algorithm == 'gcm' else req.iv
        for i, byte in enumerate(nonce): array[len(array)-8+i] = byte
        result = call(session, c.byref(req), error, 256)
        if result != 0: raise RuntimeError(error.value.decode())
        assert req.written == args.bytes + (16 if args.algorithm == 'gcm' else 0)
    try:
        for _ in range(32): run()
        before = snapshot(worker)
        caller_before = time.process_time()
        start = time.monotonic()
        count = 0
        while time.monotonic()-start < args.seconds:
            run()
            count += 1
            delay = start+count/args.pps-time.monotonic()
            if delay > 0: time.sleep(delay)
        wall = time.monotonic()-start
        caller = time.process_time()-caller_before
        after = snapshot(worker)
        print(json.dumps({'algorithm':args.algorithm, 'bytes':args.bytes, 'target_pps':args.pps,
            'pps':count/wall, 'seconds':wall, 'operations':count,
            'system_cpu_percent':100*(after['busy']-before['busy'])/(after['total']-before['total']),
            'caller_cpu_one_core_percent':100*caller/wall,
            'worker_cpu_one_core_percent':100*(after['worker_ticks']-before['worker_ticks'])/os.sysconf('SC_CLK_TCK')/wall,
            'irq_per_op':(after['irq']-before['irq'])/count,
            'ipi_per_op':(after['ipi']-before['ipi'])/count}))
    finally:
        api.destroy(session)
        c.memset(key, 0, 16)

if __name__ == '__main__': main()
