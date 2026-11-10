#!/usr/bin/env python3
"""Prepare copy-free MobileCLIP GPU profiles alongside an existing native bundle."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def digest(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('bundle', type=Path)
    p.add_argument('--compiler', type=Path, default=Path('build/src/gst/amdva-compile-model'))
    args = p.parse_args()
    root = args.bundle.resolve()
    manifest = json.loads((root / 'manifest.json').read_text())
    if digest(root / 'image.onnx') != manifest['image_sha256']:
        raise ValueError('Image ONNX checksum mismatch')
    # Match the reviewed MobileCLIP GPU preparation, except for device tensor I/O.
    env = os.environ.copy()
    passes = set(filter(None, env.get('MIGRAPHX_DISABLE_PASSES', '').split(',')))
    passes.add('simplify_algebra')
    env['MIGRAPHX_DISABLE_PASSES'] = ','.join(sorted(passes))
    env.setdefault('MIGRAPHX_GPU_COMPILE_PARALLEL', '4')
    artifacts = {}
    for batch in (1, 4):
        out = root / f'gpu-device-b{batch}.mxr'
        subprocess.run([str(args.compiler.resolve()), str(root / 'image.onnx'), str(out),
                        '--precision=fp16', f'--batch={batch}',
                        f'--input-shape=images:{batch},3,256,256'], env=env, check=True)
        for artifact in (out, Path(str(out) + '.manifest.json')):
            artifacts[artifact.name] = digest(artifact)
    meta = dict(image_sha256=manifest['image_sha256'], batches=[1, 4], precision='fp16',
                offload_copy=False, disable_passes=env['MIGRAPHX_DISABLE_PASSES'],
                artifacts=artifacts)
    (root / 'gpu-device.json').write_text(json.dumps(meta, indent=2) + '\n')


if __name__ == '__main__':
    main()
