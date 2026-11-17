<!-- source: examples/nvr/demo/1440p/README.ja.md ba45d447129224f04cbb6e14ae0b6d87b4dc4c90 -->
# Prepare the 1440p demo from Git

The standard workflow generates models on the target from locked public checkpoints, without copying prepared assets from another PC. The legacy migration section below is an auxiliary export tool, not a requirement for a fresh build.

1. Follow [installation](../../../../docs/guide/install.md) for the host SDK/framework and prepare a Ryzen AI SW 1.9 environment.
2. Supply 16 user-owned videos, one path per line. Relative paths resolve from the list file. Every video must be **2560x1440, 30fps, HEVC**. No automatic transcoding or replacement video generation is performed.
3. Prepare a new output directory:

```bash
source scripts/env.sh
python3 scripts/prepare_demo.py --videos-list /path/to/videos.txt \
  --ryzen-ai-venv /path/to/rai19-venv --backend npu \
  --output artifacts/1440p-demo
artifacts/1440p-demo/run-demo.sh
```

Add `--check-videos` to validate inputs only. CPU-only, hash-locked Python dependencies export YOLO11n, OSNet x0.25 and MobileCLIP2-S2. Source URLs and SHA-256 hashes are in `dependencies.lock.json`. GPU compilation uses host MIGraphX 2.18.0; NPU compilation happens at the final output location. Observe each model publisher's license terms.

Videos are linked rather than copied. Do not move inputs or the prepared output afterwards. Existing `config.json` is never overwritten; choose a new output for regeneration. Keep artifacts, logs, recordings and checkpoints outside Git.

GPU sidecars record GPU/SDK versions and source/generated hashes; legacy schemas require regeneration. NPU metadata records ORT, NPU identity and the loaded driver and rejects mismatches. There is no silent CPU fallback.

Desktop launch needs `XDG_RUNTIME_DIR`/`WAYLAND_DISPLAY` or `DISPLAY`. RTSP, dashboard and NVR logs are saved in the output's `logs/`; cleanup stops only owned children. There are 16 video inputs; the default wall shows 15 cameras plus the resource dashboard.

See the [support matrix](../../../../docs/dev/support-matrix.md) for reproducibility and validation status.

---

## Legacy asset export (not required for fresh construction)

### Move the 1440p, 17-input demo to another environment

Keep the templates and scripts here in Git; keep large assets and machine-local state under ignored `artifacts/`. This profile captures the camera configuration, initial layout, video order, COCO labels and deployment tooling behind `artifacts/1440p-demo`. Configuration and file verification require only Python 3.11+ and its standard library.

Defaults are 16 videos plus the resource dashboard, 16 visible tiles, detection/ReID at 3 Hz and a 3000 ms embedding interval. `cam15` is hidden to make room for the dashboard. Generated settings remain editable through the NVR.

## Files in Git and external assets

| Managed file | Purpose |
| --- | --- |
| `config.template.json` | Camera names/selections, frequencies, recording and search options |
| `layout.template.json` | Initial tile layout and stream-information setting |
| `model-info.json`, `coco80.txt` | YOLO11n 640×384 input/output description and labels |
| `videos.template.txt` | Current M01–M16 filenames in camera order |
| `manage.py`, `run.sh`, `test_manage.py` | Export, configure, verify, launch and file-contract tests |

The exporter copies only the following external runtime assets. `assets.json` records relative paths, sizes and SHA256 hashes:

- YOLO batches 1/4 and ReID batch 4, each `.mxr` with its `.manifest.json`.
- MobileCLIP `image.onnx`, `text.onnx`, `tokenizer.json` and `manifest.json`.
- GPU `gpu-device.json`, batch 1/4 `gpu-device-b*.mxr` and sidecars, when prepared.
- NPU `npu.json`, `npu-config.json` and the complete selected `npu-cache/` entry, when prepared.
- Sixteen videos, copied only when `--include-videos` is specified.

If a different GPU requires recompiling YOLO/OSNet, also transfer their original ONNX or weights separately. The exporter does not discover those source files; MobileCLIP ONNX files are included.

Old host-input `gpu-b*.mxr`, `npu-work/`, logs, recordings, SQLite databases, `.deps/`, venvs, the old comparison `libmobileclip2.so`, EmbeddingGemma and Torch are not exported for this MobileCLIP profile. Install the framework and Ryzen AI environment on the destination. Exporting an EmbeddingGemma demo is outside this profile.

## 1. Export on the source machine

Run from the repository root. The source configuration and models are only read, including when the demo is running. Choose a new destination:

```bash
python3 examples/nvr/demo/1440p/manage.py export \
  --config artifacts/1440p-demo/config.json \
  --videos-list artifacts/1440p-demo/videos.txt \
  --output artifacts/1440p-portable-assets
```

Add `--include-videos` to copy the videos too. Otherwise place all sixteen videos, with matching filenames, in a directory on the destination. `assets.json` also records their camera order.

```text
1440p-portable-assets/
  assets.json
  detector-b1.mxr             # Every mxr also has a .manifest.json
  detector-b4.mxr
  reid-b4.mxr
  mobileclip/
    manifest.json, image.onnx, text.onnx, tokenizer.json
    gpu-device.json, gpu-device-b*.mxr, ...
    npu.json, npu-config.json, npu-cache/<key>/...
  videos/                     # Only with --include-videos
```

Transfer this directory using an archive or `rsync -a`. Check out the same application commit and follow the [installation guide](../../../../docs/guide/install.md). Uncommitted application changes do not travel with a clone; commit the required changes before moving.

## 2. Prepare the destination runtime

The legacy export workflow was validated on Ubuntu 24.04, gfx1150, ROCm 7.2.0 / MIGraphX 2.15.0. The current runtime requires ROCm 10.1.0 / MIGraphX 2.18.0 and schema-3 GPU/SDK/hash manifests. Old-SDK `.mxr` files cannot be run unchanged even on the same GPU. Regenerate from original ONNX/weights using the current preparation workflow and recreate Python environments on the destination. See the [support matrix](../../../../docs/dev/support-matrix.md) for validation status.

Enable the features using the destination Ryzen AI environment, following the [MobileCLIP setup](../../../mobileclip/README.md). Stop running pipelines before building:

```bash
source scripts/env.sh
source examples/nvr/scripts/mobileclip-env.sh /path/to/target-ryzen-ai-venv
cmake -S . -B build -DAMDVA_BUILD_MOBILECLIP=ON -DAMDVA_BUILD_NPU=ON \
  -DAMDVA_ORT_ROOT="$AMDVA_ORT_ROOT" -DAMDVA_HARDWARE_TESTS=ON
cmake --build build -j8
```

### Reprepare NPU caches at their final location

The current cache's `context.json` contains absolute `/home/...` paths from preparation. Changing only the outer config does not establish NPU portability. Reprepare and validate the cache with the destination SDK, ORT, driver, NPU and final directory. There is no automatic CPU fallback.

The preparation module is `embeddinggemma2_rocm.mobileclip2_prepare` in the separate repository `https://github.com/anjn/embeddinggemma2-rocm` at commit `102a9df65cd312529668daabb3e88e0a17cfc363`. Install its preparation dependencies, move aside the copied cache and create a fresh one. Never do this to a running bundle:

```bash
bundle=/absolute/final/path/1440p-portable-assets/mobileclip
mv "$bundle/npu-cache" "$bundle/npu-cache.from-source"
PYTHONPATH=/path/to/embeddinggemma2-rocm/python \
  python -m embeddinggemma2_rocm.mobileclip2_prepare npu --bundle "$bundle"
python3 examples/nvr/demo/1440p/manage.py refresh \
  --assets /absolute/final/path/1440p-portable-assets
```

To rebuild the MobileCLIP GPU profiles, use `python3 scripts/prepare_mobileclip_device.py "$bundle"`, then run the same `refresh`. Refresh also validates model-side manifests; it does not simply approve modified `.mxr` bytes. Cache JSON is not patched by hand.

## 3. Generate local configuration and launch

```bash
python3 examples/nvr/demo/1440p/manage.py configure \
  --assets /absolute/final/path/1440p-portable-assets \
  --videos /path/to/videos \
  --venv /path/to/target-ryzen-ai-venv \
  --backend npu \
  --output artifacts/1440p-demo-local

examples/nvr/demo/1440p/run.sh --check artifacts/1440p-demo-local
examples/nvr/demo/1440p/run.sh artifacts/1440p-demo-local
```

For bundled videos, use `--videos /absolute/final/path/1440p-portable-assets/videos`. Select a prepared backend explicitly: `gpu`, `npu` or `gpu+npu`. Use `--rtsp-port 18670 --dashboard-port 18672` to change ports. CPU naming uses automatic detection unless overridden with `--cpu-name`.

`--check` verifies files, hashes and executable availability; it does not establish successful GPU/NPU inference. Before starting the destination demo, stop other pipelines and validate execution:

```bash
LD_PRELOAD="${LD_PRELOAD:+$LD_PRELOAD:}$PWD/build/src/gst/libamdva_copy_audit.so" \
  build/examples/mobileclip/amdva-mobileclip-interop "$bundle" npu 100
```

The launcher starts the video RTSP server, dashboard and NVR, and stops its own child processes on exit. Occupied ports fail before attaching to an existing demo. Use `--windowed` for a normal window. Logs go to the generated `logs/` directory. Set `QT_QPA_PLATFORM`, `WAYLAND_DISPLAY`, etc. for the destination desktop as appropriate.

Generated files are `config.json`, `config.json.ui.json`, `model-info.json`, `coco80.txt`, `videos.txt` and `demo.json`. Model/video paths are relative to this directory; the venv records its destination absolute path. Existing configs are not overwritten without `--force` (stop the demo first). Existing UI layout is preserved even with that flag.

## Moving recordings and search history

A fresh `recordings/` is used by default. To retain history, shut down the source NVR cleanly, copy its entire recording root including the database, and set the destination `recording_root`. Avoid copying only the database or a live database. Re-preparing models/NPU caches may change the search namespace, making previous embeddings unavailable under the new configuration. There is no automatic re-embedding.

## File-transfer contract

```bash
python3 examples/nvr/demo/1440p/test_manage.py -v
```

Small fake files test relocation, paths containing spaces, video order, tamper detection, overwrite protection, layout preservation and inventory refresh after recompilation. No GPU/NPU or running demo is used. CMake registers this as `demo-portability-contract`.

## Thirty-minute hardware acceptance

```bash
artifacts/1440p-demo/run-demo.sh --soak-seconds 1800
```

Runs the same GUI/backend and logs channel, recording, search, RSS, FD and copy-audit metrics every five seconds to `logs/soak.jsonl`. Owned children are stopped on exit. Failure includes post-warm-up median RSS growth above the larger of 512 MiB or 25%, FD growth above 64, channel errors, audit violations, or search not exercised. Videos without people cannot establish search acceptance. Keep the desktop available; locking it or closing the app affects validation.

Before a new launch, previous `logs/` are preserved under `logs.<UTC timestamp>/`. `--check` does not rotate logs or start processes.

## GPU measurement mode

The demo defaults to the conventional `sysfs` GPU metric. Following a report of
instability during extended demo operation, wave collection is disabled by
default; its involvement in the instability has not been established.
`AMDVA_GPU_WAVE_MODE=window artifacts/1440p-demo/run-demo.sh` explicitly enables
the diagnostic wave mode (500 ms collection / 500 ms pause), requiring
`amdva-wave-sampler` and a hardware-validated OS/GPU pair. Wave presentation uses an exponential average with
a 3-second time constant (about 9 seconds to follow 95% of a sustained change).
Invalid readings hold the display for at most 2 seconds, then become unavailable;
sysfs values are not mixed into the wave average. `gpu_source` and `gpu_status`
report the display source and state; `gpu_raw_pct`, `gpu_raw_source` and
`gpu_raw_status` retain the original reading. The candidate includes resident waves waiting for memory and measures
a cycle ratio, not the fraction of additional work that can be admitted.

Set `AMDVA_GPU_WAVE_TIMING=1` to log counter API start times (monotonic nanoseconds), durations and return statuses to `/run/user/<uid>/amdva-wave/<PCI-address>.log`. This is disabled by default. Durations measure CPU call time, not an upper bound on perturbation of concurrent GPU work. To also retain per-camera status, select an unused `AMDVA_DIAGNOSTICS_DIR`; `AMDVA_DIAGNOSTICS_TRACE=0` disables detailed frame tracing.
