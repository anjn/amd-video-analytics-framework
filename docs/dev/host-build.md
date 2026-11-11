# Building on the host and running the reference pipeline

The user guide has the short version ([install](../guide/install.md), [quickstart](../guide/quickstart.md)). This page keeps the full host notes, the dependency lock and the reference runner with the copy auditor.

## Build on the host

Use host-installed ROCm **10.1.0** and MIGraphX **2.18.0** on Ubuntu 24.04 or 26.04.
The two SDK roots may differ. The framework owns GStreamer and its patches; the
host owns Mesa/libva/libdrm, Qt and native development packages. Do not reuse the
old `.deps/rocm72` or cross-distribution `.deps/sysroot`.

Follow [installation](../guide/install.md), then run `python3 scripts/host.py --write`
and `python3 scripts/bootstrap.py --jobs 8`. With NPU, pass the host-prepared
`--ryzen-ai-venv` to both checks/build. No SDK installation or sudo is performed
by bootstrap. `scripts/env.sh` exposes only the recorded host SDK and framework prefix.

See the [support matrix](support-matrix.md) for measured vs pending configurations.
Model manifests use schema 3 and record the GPU, ROCm, MIGraphX, input ONNX hash,
precision, batch and generated artifact hash. Legacy schema 1/2 models require
regeneration. No serialized MIGraphX artifact is assumed compatible across SDKs.

## Prepare a model and run

Use a static YOLOv8 COCO ONNX with `[1,3,640,640]` RGB input and
`[1,84,8400]` output. Model metadata follows the supplied
[example](../../examples/yolov8n.json); the 80-line label file sits beside that JSON.
Models and label files from the background project can be used directly.

```bash
source scripts/env.sh
mkdir -p artifacts
build/src/gst/amdva-compile-model /path/yolov8n.onnx artifacts/yolov8n.mxr \
  > artifacts/model-compile.log 2>&1
LD_PRELOAD="$PWD/build/src/gst/libamdva_copy_audit.so" \
  build/src/gst/amdva-run /path/input.h264 artifacts/yolov8n.mxr \
  /path/yolov8n.json --events=artifacts/events.jsonl
```

Model preparation uses FP16 internal computation and `offload_copy=false`.
It writes a SHA256 and GPU identity manifest beside the MXR; runtime rejects a
missing, mismatched or incompatible manifest. Pre-existing MXR files must be
recompiled. Model compilation is separate from runtime pipeline control.

`amdva-run` reports frame counts, detection counts, P95/P99 processing latency,
original-memory/PTS checks and copy-audit results. `--amd-only` skips DL Streamer
for diagnosis. Without the audit library the report explicitly leaves zero-copy
verification false. `--duration=SECONDS` repeats a 30fps elementary H.264 fixture
at clock pace; SIGINT/SIGTERM requests EOS and drains the pipeline.

```bash
python3 tools/soak.py /path/input.h264 artifacts/yolov8n.mxr /path/yolov8n.json \
  --seconds=3600 --output=artifacts/soak
```

The soak collects RSS, FD and allocated GPU-memory samples, checks bounded growth after warm-up, and
requires DL Streamer integration plus copy-audit success. See
[validation](validation.md) for acceptance thresholds and reproduction.

Video recording preserves compressed RTSP packets; the framework does not
provide an NPU inference backend. The [resource dashboard sample](resource-dashboard/README.md)
uses hardware H.264 encoding for GPU-rendered monitoring graphics.
