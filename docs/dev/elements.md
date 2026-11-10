# Element reference

All elements are in the `amdanalytics` plugin (`libgstamdanalytics.so`, rank NONE,
so they are never auto-plugged) unless marked *vendored* or described as a separate
plugin. Properties marked
*READY* can be changed only in NULL/READY. Run `gst-inspect-1.0 <element>` for the
authoritative list on your build.

Set `GST_PLUGIN_PATH` to the plugin directory (`build/plugins` or
`<prefix>/lib/gstreamer-1.0`).

## Inference: `amddetect`, `amdclassify`, `amdinference`

One implementation, three defaults. In place; adds metadata, no pixels change.

| | `amddetect` | `amdclassify` | `amdinference` |
| --- | --- | --- | --- |
| Default `inference-region` | `full-frame` | `roi-list` | `full-frame` |
| Tasks | detection, instance segmentation, pose | classification | any `task_type` (semantic segmentation, raw tensors, ...) |

Pads (sink and src): `video/x-raw(memory:VAMemory),format=NV12,width=[2,32766],height=[2,32766]`
or `multistream/x-analytics-batch(meta:GstAnalyticsBatchMeta)` (the container
produced by `gvastreammux`). The frame must be BT.709 limited range with even
dimensions.

| Property | Type / range | Default | Meaning |
| --- | --- | --- | --- |
| `model` | string | | Compiled `.mxr` model with its `.manifest.json` (READY) |
| `model-info` | string | | [model-info](model-info.md) JSON (READY) |
| `model-instance-id` | string | | Elements with the same ID, model and settings share one executor and batch together |
| `inference-region` | `full-frame` \| `roi-list` | see above | `roi-list` crops up to 300 upstream `GstAnalytics` boxes on the VPE |
| `device` | 0-16 | 0 | HIP device index |
| `batch-size` | 1-16 | 1 | Must equal the compiled batch |
| `batch-timeout` | 0-10000 ms | 20 | Wait for a partial batch |
| `inference-interval` | 1-100000 | 1 | Infer every Nth frame |
| `confidence-threshold` | 0-1 | 0.25 | Minimum object score |
| `nms-threshold` | 0-1 | 0.45 | NMS IoU |
| `require-zero-copy` | bool | true | Reject any path that copies images or tensors on the CPU |

Metadata attached: `GstAnalytics` object detection, classification and keypoint
group mtds, plus `AmdAnalysisMeta` for GPU masks/tensors, inverse geometry and
frame identity. Queue overflow is an error, not unbounded buffering.

## `amdwatermark`

Draws boxes, labels, tracking IDs, instance/semantic masks and COCO-17 skeletons
on the GPU.

| | |
| --- | --- |
| Sink | `video/x-raw(memory:VAMemory),format=NV12,width=[2,4096],height=[2,4096]` |
| Src | `video/x-raw(memory:DMABuf),format=BGRA,width=[2,4096],height=[2,4096]` (linear, exported VA surface) |
| `device` | HIP device index (default 0, READY) |
| `require-zero-copy` | default true (READY) |

The BGRA surface can be displayed through EGL (`amdva::EglDmaBufImporter`) or
encoded with `amdencode`. Plain `vapostproc` cannot negotiate it.

## `amdencode`

VA hardware encoder bin; see [encoding and serving](encode-and-serve.md).

| | |
| --- | --- |
| Sink | `video/x-raw(memory:VAMemory),format={NV12,BGRA,BGRx,RGBA}` or `video/x-raw(memory:DMABuf),format={NV12,BGRA,BGRx,RGBA}` |
| Src | `video/x-h264` or `video/x-h265` byte-stream, AU-aligned; `video/x-av1` OBU/TU |
| `codec` | `h264` (default), `h265`, `av1` |
| `bitrate` | 1-2048000 kbit/s (default 4000) |
| `key-int-max` | 1-10000 (default 30) |
| `b-frames` | 0-4 (default 0; not for AV1) |
| `signal-bt709` | default true; H.264 SPS VUI rewrite |

All properties READY. Internally: DMA-BUF import and conversion stage,
`vapostproc`, caps filter, `vah264enc`/`vah265enc`/`vaav1enc`, parser.

## `amdpython`

Metadata-only callback; see [metadata and Python](metadata-and-python.md).

| Property | Default | |
| --- | --- | --- |
| `module` | | Python source file (READY) |
| `function` | `process` | `callback(frame_dict)` (READY) |

Caps: any (pass-through).

## `amdmetapublish`

Bounded asynchronous publisher; caps any, pass-through.

| Property | Default | |
| --- | --- | --- |
| `method` | `none` | `none`, `file`, `mqtt`, `kafka` |
| `file-path` | | JSON lines destination |
| `address` | | Broker address |
| `topic` | `amdva/analytics` | |
| `queue-limit` | 256 | 1-65536 records |
| `delivered`, `dropped`, `errors` | read-only | uint64 counters |

## Compatibility pair: `amdpreproc`, `amdmigraphx`

The original two-element YOLOv8 interface, retained for compatibility
(`amdpreproc`: `device`, `tensor-width`, `tensor-height`, `model-info`,
`require-zero-copy`, `output-memory`; `amdmigraphx`: `model`, `model-info`, `device`,
`confidence-threshold`, `nms-threshold`, `require-zero-copy`). New GPU task pipelines should
use `amddetect`; do not put `amdpreproc` in front of the task elements.

## NPU inference: `amdnpu`

The optional `amdnpu` plugin (`libgstamdnpu.so`, `AMDVA_BUILD_NPU=ON`) uses
ONNX Runtime's Vitis AI EP in the same process as HIP/MIGraphX. Source
`scripts/env.sh` and `examples/nvr/scripts/mobileclip-env.sh RYZEN_AI_VENV`
before starting the process. The build needs `AMDVA_ORT_ROOT` pointing to the
Ryzen AI C++ SDK. Disabling NPU leaves GPU elements available without its SDK.

Both pads accept `application/x-amd-shared-tensor` or
`video/x-raw(memory:VAMemory),format=NV12,width=[2,32766],height=[2,32766]`.
Each buffer must carry `AmdSharedTensorMeta`. Raw video alone is an error.
The element preserves caps, original GstMemory, timestamps and input metadata.
It performs one synchronous inference per buffer and adds `AmdNpuResultMeta`.

| Property (all READY) | Meaning |
| --- | --- |
| `model` | Image ONNX model; one static float32 `[1,3,H,W]` input |
| `provider-config` | Vitis AI provider JSON |
| `cache-dir`, `cache-key` | Offline-prepared cache root and key; `context.json` must exist |
| `gpu-device` | HIP producer device, default 0 |

The named free dimension `batch` is fixed to 1; other dynamic dimensions are
rejected. CPU EP fallback is disabled. Outputs are named, dense numeric tensors
with static shapes and float32, float16, int32, int64 or uint8 element types.
There is no model-specific postprocessing. Initialization and inference failures
post bus errors; properties cannot be changed in PAUSED/PLAYING.

`amdva::SharedTensor` owns one HIP mapped host allocation, its GPU alias and a
producer completion event. Write through `device_data()`, then call
`record_ready(stream)`. Attach it with `add_shared_tensor_meta(buffer, tensor)`.
The consumer waits for readiness and creates the ORT input over `host_data()`;
`NpuResult::input_address` records that exact pointer. Retain the allocation and
avoid writing it again until inference and every consumer of the input finish.

`NpuResult::tensors` supplies descriptors, read-only data pointers and shared
owners of the ORT output allocations. Results survive subsequent inference calls
and pipeline stop without copying the raw outputs. Full GstBuffer copies share
input/output ownership; region copies omit both metas. This contract excludes
EP-internal transfers and cache synchronization.

`amdpreproc output-memory=host-mapped` writes HIP preprocessing output directly
into a new shared allocation and attaches `AmdSharedTensorMeta`. Its default
`output-memory=device` retains the original GPU tensor behavior. Model-info
controls the input name, dimensions and normalization in either mode.

```text
filesrc location=in.mp4 ! qtdemux ! h265parse ! vah265dec !
  amdpreproc output-memory=host-mapped model-info=image.json !
  amdnpu model=image.onnx provider-config=provider.json cache-dir=cache cache-key=model !
  appsink
```

Native tensor producers can use `AMDVA::amdva_npu_pipeline` and
`amdva::NpuPipeline` for a persistent `appsrc → amdnpu → appsink` pipeline. Calls
are serialized and both application queues are bounded to one buffer. A failed
or timed-out pipeline must be recreated. MobileCLIP and NVR use this adapter for
NPU image inference; their Pillow-compatible HIP preprocessing remains in the
native MobileCLIP library. CPU text inference and GPU image inference retain
their existing paths and search fingerprints.

### Reproduce the hardware checks

```bash
source scripts/env.sh
source examples/nvr/scripts/mobileclip-env.sh /path/to/ryzen-ai-venv
cmake -S . -B build -DAMDVA_BUILD_NPU=ON -DAMDVA_HARDWARE_TESTS=ON \
  -DAMDVA_ORT_ROOT="$AMDVA_ORT_ROOT"
cmake --build build -j8
python scripts/prepare_npu_fixture.py artifacts/npu-fixture /path/to/provider.json
build/src/gst/amdva-validate-npu artifacts/npu-fixture /path/to/hevc.mp4
```

The fixture is a static Conv/Relu/GlobalAveragePool model with two raw outputs,
independent of MobileCLIP. The validator alternates GPU-written inputs for 100
calls, checks analytic output parity and input pointer identity, retained output
after stop, metadata copies, rejected inputs, READY properties, caps, flush/EOS
and optional VA decode/preprocessing. Set `AMDVA_NPU_TEST_FIXTURE` and
`AMDVA_RYZEN_AI_TEST_VENV` to register it as `npu-element-contract` in CTest.
The CPU-only `npu-meta-contract` checks descriptors and output ownership even
with NPU disabled. See [recorded validation](npu-element-results.json) and the
[MobileCLIP sample](../../examples/mobileclip/README.md) for GPU/NPU coexistence.

## Vendored DL Streamer elements

Built from `third_party/dlstreamer` (`AMDVA_BUILD_GVA_COMPAT`). Properties are those of DL Streamer v2026.2.0.

| Element | Use here |
| --- | --- |
| `gvatrack` | `tracking-type=short-term-imageless` or `zero-term-imageless`; CPU-only, no image mapping |
| `gvametaconvert` | GstAnalytics to DL Streamer JSON (`format=json`, `json-indent`, `source`, `tags`) |
| `gvametapublish` | `method=file|mqtt|kafka`, `file-path`, `address`, `topic` |
| `gvastreammux` | `output-mode=container` for mixed-resolution multi-stream batching; `max-queue-size`, `max-wait-time`, `sync-mode` |
| `gvastreamdemux` | splits the container back to per-source branches |
| `gvametaaggregate` | merges parallel branches; merges AMD GPU payloads |

## Typical pipeline

```text
filesrc ! h264parse ! vah264dec ! video/x-raw(memory:VAMemory),format=NV12 !
  amddetect model=m.mxr model-info=m.json !
  gvatrack tracking-type=short-term-imageless !
  amdpython module=cb.py ! amdmetapublish method=mqtt address=tcp://broker:1883 !
  amdwatermark ! amdencode ! h264parse ! mp4mux ! filesink location=out.mp4
```
