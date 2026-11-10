#!/usr/bin/env python3
"""Exercise native MobileCLIP GPU/NPU contexts in a fresh process per order.

Run with the framework and Ryzen AI environments sourced; see npu-interop.md.
Input/output use host buffers. This is a coexistence test, not a zero-copy test.
"""
import argparse
import ctypes as C
import json
import math
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", required=True)
    parser.add_argument("--bundle", required=True)
    parser.add_argument("--order", choices=("hip-first", "gpu-first", "npu-first"), required=True)
    args = parser.parse_args()
    lib = C.CDLL(args.library)
    lib.mc2_create.argtypes = [C.c_char_p, C.c_char_p, C.POINTER(C.c_void_p), C.c_char_p, C.c_size_t]
    lib.mc2_prepare.argtypes = [C.c_void_p, C.c_char_p, C.c_size_t]
    lib.mc2_encode_pixels.argtypes = [C.c_void_p, C.POINTER(C.c_float), C.c_size_t,
                                    C.POINTER(C.c_float), C.c_size_t, C.c_char_p, C.c_size_t]
    lib.mc2_destroy.argtypes = [C.c_void_p]
    lib.mc2_destroy.restype = None
    contexts = []
    error = C.create_string_buffer(4096)

    def checked(rc, stage):
        if rc:
            raise RuntimeError(f"{stage}: status={rc} {error.value.decode(errors='replace')}")
        print(json.dumps({"stage": stage, "passed": True}), flush=True)

    try:
        if args.order == "hip-first":
            hip = C.CDLL("libamdhip64.so")
            hip.hipInit.argtypes = [C.c_uint]
            checked(hip.hipInit(0), "hipInit")
        backends = ("gpu", "npu") if args.order == "gpu-first" else ("npu", "gpu")
        for backend in backends:
            ctx = C.c_void_p()
            checked(lib.mc2_create(args.bundle.encode(), backend.encode(), C.byref(ctx),
                                   error, len(error)), f"{backend} create")
            contexts.append(ctx)
            checked(lib.mc2_prepare(ctx, error, len(error)), f"{backend} prepare")
        runtimes = sorted({line.split()[-1] for line in Path("/proc/self/maps").read_text().splitlines()
                           if any(name in line for name in ("libamdhip64", "libhsa-runtime64",
                                                             "libxrt_coreutil", "libxrt_driver_xdna",
                                                             "libonnxruntime.so"))})
        print(json.dumps({"runtime_libraries": runtimes}), flush=True)
        pixels = (C.c_float * (3 * 256 * 256))(*([0.5] * (3 * 256 * 256)))
        for backend, ctx in zip(backends, contexts):
            out = (C.c_float * 512)()
            checked(lib.mc2_encode_pixels(ctx, pixels, 1, out, 512, error, len(error)),
                    f"{backend} inference with both contexts alive")
            norm = math.sqrt(sum(v * v for v in out))
            if not math.isfinite(norm) or abs(norm - 1) > 1e-4:
                raise RuntimeError(f"{backend}: invalid embedding norm {norm}")
            print(json.dumps({"backend": backend, "embedding_norm": norm}), flush=True)
    finally:
        for ctx in reversed(contexts):
            lib.mc2_destroy(ctx)


if __name__ == "__main__":
    main()
