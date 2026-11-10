<!-- source: examples/mobileclip/README.ja.md c369f5bc45a0c7f6b53eb231312045e43012f4cb -->
# MobileCLIP GPU and NPU in one process

This sample and the NVR share `amdva_mobileclip`. GPU preprocessing writes
float32 NCHW tensors directly into the inference input. GPU inference uses
MIGraphX device I/O; NPU inference wraps HIP-mapped host memory in an ONNX Runtime
CPU tensor and invokes Vitis AI EP. The EP's internal buffer handling is outside
this application's copy-free contract. No XRT calls are required by the sample.

NPU image inference uses a persistent GStreamer pipeline,
`appsrc → amdnpu → appsink`. MobileCLIP-specific HIP preprocessing stays in the
native library and passes a shared tensor to the element. The generic ONNX
element retains raw named, typed, shaped outputs in metadata. This transport
change preserves search fingerprints. See [element and generic-model
validation](../../docs/dev/elements.md#npu-inference-amdnpu).

With `AMDVA_BUILD_NPU=OFF`, MobileCLIP GPU image inference and CPU text inference
remain available. Selecting NPU in that build reports an initialization error.

## Build and prepare

Use an existing MobileCLIP2-S2 native bundle containing `image.onnx`, `text.onnx`,
`tokenizer.json`, checksums and, for NPU, the prepared BF16 `npu-cache` and
`npu.json`. Keep the existing bundle in its preparation environment; copy it
into this checkout's ignored `artifacts/` directory before adding GPU profiles.
Preparation is offline; the applications do not compile models on startup.

```bash
source scripts/env.sh
source examples/nvr/scripts/mobileclip-env.sh /path/to/ryzen-ai-venv
cmake -S . -B build -DAMDVA_BUILD_MOBILECLIP=ON -DAMDVA_BUILD_NPU=ON -DAMDVA_ORT_ROOT="$AMDVA_ORT_ROOT"
cmake --build build -j8
cp -a /path/to/mobileclip2-api artifacts/mobileclip-device
python scripts/prepare_mobileclip_device.py artifacts/mobileclip-device
```

The optional build needs the Ryzen AI ONNX Runtime C++ SDK and Cargo.
The vendored MIT tokenizer and its Cargo dependencies are pinned in
`third_party/mobileclip-tokenizer` and `dependencies.lock.json`.
The framework's ordinary build can leave `AMDVA_BUILD_MOBILECLIP=OFF`;
selecting MobileCLIP in that build reports an error.
GPU profiles `gpu-device-b1.mxr` and `gpu-device-b4.mxr` are compiled with FP16
operations and `offload_copy=False`. The old `gpu-b*.mxr` host-I/O profiles are
kept for reference and are never loaded by this runtime.

## Run the proof

```bash
LD_PRELOAD="$PWD/build/src/gst/libamdva_copy_audit.so" \
  build/examples/mobileclip/amdva-mobileclip-interop \
  artifacts/mobileclip-device gpu+npu 100 /path/to/reference/libmobileclip2.so
```

The arguments are bundle, mode (`gpu`, `npu`, or `gpu+npu`), iteration count
(at least 100), and optional old native library for output parity. The source
images are generated on the GPU. Every run varies image dimensions and alternates
contents. GPU batches cover 1–4 images (partial batches pad to four); NPU uses
one. The two backends execute concurrently in combined mode. GPU preprocessing
is checked against the old Pillow-compatible CPU algorithm, and optional
embedding parity requires cosine similarity at least 0.9999 for each backend.
English and Japanese text queries use the same CPU tokenizer/model as before and are compared with the old library when provided. Invalid dimensions, device IDs, empty inputs/text, and cross-runtime prepared inputs must be rejected.

The audit library is mandatory for this proof. The JSON output includes PID,
GPU-written input pointer, ORT-bound pointer, repeat count, buffer reuse,
preprocessing error, input-copy violations, reference parity and mean call time.
`application_input_copy_verified=true` requires no copies in the GPU-input
handoff, exact preprocessing parity, and buffer reuse. Diagnostic tensor
readbacks and old reference inference are explicitly exempted; they are not
part of the application inference path. The timing includes preparation and
inference; its average is not an NVR throughput guarantee.

`ep_internal_copy_verified` remains false. The audit covers application HIP
transfers during input preparation and checks that the original allocation is
bound unchanged. It does not trace arbitrary CPU memcpy inside ONNX Runtime,
Vitis AI, or MIGraphX. Cache coherence and NPU internal DMA are handled by the EP.

## NVR configuration

```json
{
  "embedding_model": "mobileclip2-s2",
  "mobileclip_backend": "gpu+npu",
  "mobileclip_bundle": "/absolute/path/to/mobileclip-device",
  "embedding_batch": 4,
  "ryzen_ai_venv": "/path/to/ryzen-ai-venv"
}
```

Place these fields inside `search`. The checkout launcher reads the selected
model and configures provider dependencies before starting the native process:

```bash
examples/nvr/scripts/run.sh --fullscreen /path/to/nvr-config.json
```

The launcher uses `search.ryzen_ai_venv`, or `AMDVA_RYZEN_AI_VENV` / the activated
`VIRTUAL_ENV`. It removes inherited EmbeddingGemma/PyTorch library paths before
setting up MobileCLIP. Leaving Torch's HIP aliases or bundled libtinfo in the
search path can prevent Vitis AI from loading and also prevent ReID startup.
The prepared local `artifacts/1440p-demo/run-demo.sh` uses this launcher.

All MobileCLIP inference is in process. The old `mobileclip_library`,
`mobileclip_worker` fields remain readable for configuration compatibility but do not control execution. `ryzen_ai_venv` is used by the launcher for environment setup, not to start a worker. Neither a worker executable nor
Python is used for inference. The environment helper runs before startup;
changing a library path after HIP/ORT initialization is not supported.

NVR retains each GPU crop independently across frames, pending replacements,
and asynchronous jobs. CPU thumbnails remain available but are not
fed back into MobileCLIP. ReID preprocesses the original VA frame and person ROI
through `amdpreproc` on the GPU, independently of those CPU images. CPU-origin images supplied through the search API
are uploaded once at ingestion. NPU preprocessing briefly acquires the GPU gate,
then releases it before the EP call. GPU batch preferences, independent device
budgets, text search and per-backend namespaces remain available. The new
fingerprint distinguishes these profiles from the old worker-based index;
there is no database migration or automatic re-embedding of old recordings.

See [recorded validation](../../docs/dev/mobileclip-native-results.json) and
[the earlier XRT investigation](../../docs/dev/npu-interop.md).

For a headless NVR integration check with an already running RTSP fixture server:

```bash
LD_PRELOAD="$PWD/build/src/gst/libamdva_copy_audit.so" \
  build/examples/nvr/amdva-mobileclip-nvr-validate /path/to/nvr-config.json
```

This tool is built with `AMDVA_HARDWARE_TESTS=ON`. It checks collection, English
and Japanese queries, a CPU-origin image query, runtime budget changes,
absence of child processes, copy auditing and shutdown. The GUI search validator
also covers live/recorded region queries, playback and recording deletion.
