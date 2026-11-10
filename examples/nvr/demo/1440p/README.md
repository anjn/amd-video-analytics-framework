<!-- source: examples/nvr/demo/1440p/README.ja.md 94b0d9fb3ab85bb2ab997d9e829dc9813cb8829b -->
# Move the 1440p, 17-input demo to another environment

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

The validated environment is Ubuntu 24.04, gfx1150, ROCm 7.2.0 / MIGraphX 2.15.0. Compiled `.mxr` programs depend on GPU architecture and runtime libraries. For a different GPU/ROCm, recompile from the original ONNX/weights; see [model preparation](../../../../docs/dev/nvr/nvr-ai-performance.md). Recreate the destination Python environment rather than copying a venv.

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
LD_PRELOAD="$PWD/build/src/gst/libamdva_copy_audit.so" \
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
