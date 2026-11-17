# Desktop NVR sample

`amdva-nvr CONFIG.json` records every registered RTSP source and displays up to
16 selected sources in a Qt desktop window. YOLOv8n runs on the selected sources
with a shared MIGraphX batch executor. Hidden cameras use optional keyframe sampling or continuous decoding/inference when ReID is configured. Source registration has no fixed count
limit; camera, storage, network and decoder capacity still limit a deployment.
The supported input format is progressive, even-sized, 8-bit NV12 up to
2560×1440, BT.709 limited range, from H.264 or H.265 at 30 FPS. Audio is ignored.

## Build and start

Install the host prerequisites and record them with `python3 scripts/host.py --write` as described in [Host build](../host-build.md). ROCm 10.1.0, MIGraphX 2.18.0, Mesa/libva/libdrm, Qt and SQLite development packages come from the validated host. `python3 scripts/bootstrap.py` builds pinned GStreamer 1.28.2 and its patches under `.deps`, then configures/builds the framework; it does not install an alternate GPU SDK or Qt. Always source `scripts/env.sh` before configuring, building or running. `-DAMDVA_BUILD_NVR_GUI=OFF` keeps the backend and headless `amdva-streams` without Qt; `-DAMDVA_BUILD_NVR=OFF` skips the sample. An installed SDK also supports standalone configuration: `cmake -S examples/nvr -B build-nvr -DCMAKE_PREFIX_PATH=/path/to/amdva-sdk`.

Prepare the same YOLOv8n COCO weights in a separate CPU-only export environment:

```bash
source scripts/env.sh
python3 scripts/setup_model_export.py
export PATH="$AMDVA_ROOT/build/src/gst:$PATH"
.deps/model-export/bin/python examples/nvr/scripts/export_nvr_model.py \
  /path/to/yolov8n.pt artifacts/nvr-model --batch=4
cp examples/nvr/config/nvr.json artifacts/nvr-local.json
# Edit paths and camera URLs in the copied config; paths are relative to that config.
WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland-egl \
  build/examples/nvr/amdva-nvr artifacts/nvr-local.json
```

Alternatively run under an X11 desktop with `QT_QPA_PLATFORM=xcb`; the environment
selects the EGL integration. An AMD hardware OpenGL ES 3 context and DMA-BUF
import are required. Software rendering is rejected. No CPU image upload fallback
is provided.

Add `--fullscreen` before the config path to start in full-screen mode, for
example for demonstrations. F11 switches between full-screen and windowed mode.

Use **Live** to assign cameras to fixed 1/4/9/16-view tiles and double-click a
tile for single-view focus with a coverage timeline, in-place hover preview,
recording playback and a Back to Live action. **Cameras** provides add/edit/remove,
status, retention settings and recording deletion. Every
registered camera records regardless of its live-view selection. See the
[VMS GUI guide](vms.md) for operation, playback boundaries and catalog migration.
Configuration changes are saved atomically with owner-only permissions; the
UI arrangement is stored separately in `<config>.ui.json`. Status messages omit
URLs that may contain credentials.

The config keys are demonstrated in `examples/nvr/config/nvr.json`. Batch size is one of
1, 2, 4, 8 or 16; `model` must match that size and `single_model` must be a
batch-1 model for partial batches of one. Model-info describes one image. Compiled models require a schema-3
manifest with matching GPU, ROCm/MIGraphX versions, SHA-256 and `offload_copy=false`, plus the batch size. Legacy schema-1/2 artifacts must be regenerated on the target host. Dynamic-batch ONNX is exported from the
weights; changing a fixed-batch ONNX input alone is insufficient.

Each desktop launch clears previous catalogued recordings and search data before reception. Unmanaged files remain. `amdva-streams` and SDK controllers do not clear history automatically. See [recording management](vms.md#camera-management-and-storage).

## Layout, shared services, tracking and publication

| Directory | Contents |
| --- | --- |
| `backend/` | Qt-free `amdva_nvr_backend` library (`amdva::nvr`): camera controller, config persistence, AI scheduling (`AiRuntime`), person search and tracking, in-process MobileCLIP runtime |
| `app/` | Qt desktop (`amdva-nvr`) |
| `headless.cpp` | `amdva-streams`, the backend without a display |
| `tools/`, `tests/`, `scripts/` | validation programs, contract tests, model export and benchmarks |
| `config/`, `benchmarks/` | example configurations and reviewed fixtures; guides and validation records live in `docs/dev/nvr/` |

The backend uses only the public framework: RTSP ingest, segment recording,
the recording catalog, keyframe inspection and playback come from
`amdva_streaming`. The person-search table lives in the recording catalog's
database through `Catalog::extend`, so its rows are removed with recordings and
counted in the quota. GPU image packing, batch execution, imageless tracking,
overlay geometry and result publication use shared framework libraries. See
[framework interfaces](../framework.md) for SDK and headless usage.

`tracking_enabled` defaults to true and enables imageless DL Streamer tracking for displayed frames. Configured ReID supplies confirmed person IDs to the live overlay independently; it uses its own sampled person tracker. Reconnects, selection changes and incompatible caps reset imageless tracking. Without ReID, hidden keyframe detections are independent untracked results.

Optional `results` configuration uses the same asynchronous publisher as
`amdmetapublish`:

```json
"results": {
  "method": "file", "file_path": "events.jsonl",
  "address": "", "topic": "amdva/analytics",
  "queue_limit": 256, "byte_limit": 4194304
}
```

Use `method=none` to disable output (default), `mqtt` with an address such as
`tcp://127.0.0.1:1883`, or `kafka` with `127.0.0.1:9092`. JSON contains selected
tracked observations and background observations with source generation and
PTS. File paths are relative to the config. Full queues discard oldest waiting
records; delivery errors do not stop recording/display. The UI reports delivered,
dropped and error counts. Output is best effort and is not a durable event log.

## Background AI for hidden sources

`background_ai_enabled` controls analysis of cameras outside the active live layout. It defaults to false when omitted; the example config enables it. Enabling `search` forces background AI on. A nonempty `search.reid_model` selects continuous tracking across visibility changes.

With ReID configured, hidden cameras use continuous hardware decoding and the requested `ai_fps`, rather than periodic keyframe snapshots. Sixteen dedicated hidden-camera slots are available in addition to sixteen visible slots. Admission beyond hidden capacity waits for a free slot. This is scheduling capacity, not a throughput guarantee. Embedding initialization/runtime failures do not stop tracking; tracking and embedding report separate readiness/errors.

Without ReID, `background_ai_interval_seconds` (integer 1–3600, default 5) selects the next independently decodable access unit after each deadline. H.264 accepts IDR; H.265 accepts IDR, BLA and CRA. Cached parameter sets are bounded to 1 MiB per source and slice references are validated. Ordinary I pictures or the absence of `DELTA_UNIT` alone are insufficient. A fresh VA decoder receives the cached parameters and one AU, then EOS; a two-second timeout affects background AI only. There is no full-stream or CPU decode fallback. Cameras already using a transcoder reuse its decoded frames.

The controller owns sixteen background slots; keyframe-only operation skips admission when all are occupied. Background and visible images share the batch executor. A mixed batch admits at most one background image; background-only batches can fill all positions. Selection, removal and reconnection invalidate obsolete jobs by generation. Background boxes are not reused as live overlays.

The camera status and telemetry expose background state, inference/detection counts and result age. Periodic background detections alone do not create a persistent event database. Configured search stores eligible person observations in the catalog; configured result publication can export background observations. Recording remains independent of background sampling.

## Pipelines and failure handling

The ordinary pass-through recording path separates reception, recording and decoding. The diagram is schematic; re-encoded recording uses `Transcoder` and shares its decoder output with analytics/display:

```text
rtspsrc → depay → parse → compressed appsink
                       ├─ buffer references → recorder appsrc → parse → splitmuxsink
                       └─ visible / continuous ReID → decoder appsrc → VA decoder → VAMemory appsink
                                                                  ├─ presentation FIFO → VPE → EGL tiles
                                                                  └─ VPE → HIP → batch MIGraphX → GPU NMS
```

Compressed queues are limited to 16 MiB and approximately two seconds per
receiver. Appsrc queues are also bounded. No source callback waits for disk I/O,
AI or the GUI. A compressed backlog restarts the affected recording or decoder
at a keyframe instead of dropping dependent pictures silently. Recording errors
and quota exhaustion are visible; reception and selected display continue.

RTSP uses TCP, a 100 ms jitter latency and a five-second no-video watchdog.
Reconnect delays grow from one to thirty seconds with jitter and repeat until
the source is removed. Every reconnect gets a new generation and recording
session. Decoder format changes are renegotiated; unsupported formats produce a
visible decoder error. Old-generation detections are rejected. Resolution
changes invalidate overlays, and results older than 500 ms are hidden.

Recording is the original encoded video, with parser framing converted as
required by Matroska. Files split at keyframes around `segment_seconds`, so a
long GOP can make a segment longer than the configured target. Filenames contain
UTC epoch microseconds, channel ID, session UUID and fragment number. Window close, SIGINT and SIGTERM perform graceful shutdown. Shutdown
sends EOS and allows up to five seconds for asynchronous muxers to finish.

SQLite catalogues files created by this application. The recorder root has an
exclusive writer lock. Once the managed files exceed `quota_gib` or available
space drops below `reserve_gib`, the oldest closed managed files are deleted.
Active recordings, unregistered files and symlinks are excluded. If no eligible
files can restore capacity, recording pauses and resumes at a keyframe when
space becomes available. After an abrupt shutdown, previous active entries
become deletion candidates; they are not claimed to be valid finalized MKVs.

`retention_minutes` defaults to 60 and removes inactive files by their known
end time; 0 disables time-based removal. Files spanning the cutoff or with
unknown duration are excluded. Playback/extraction handles protect files until
released. The [VMS recording controls](vms.md#camera-management-and-storage)
provide retention editing and confirmed deletion of closed recordings.

## GPU ownership and scheduling

Startup requires an observed increase of the dedicated VPE engine counter.
The application keeps one shared GstVaDisplay and 16 persistent view slots.
Preview and AI use separate VA processing contexts. The preview context is renewed
when source or output geometry changes; AI does the same on source geometry changes.
Preview downscaling is limited to the validated 6:1 range, with further window
reduction performed by GL sampling. A minimized window therefore does not submit
extreme reduction ratios to VPE.

Each slot owns a 640×640 linear BGRA VPE output with a persistent HIP mapping,
plus three 2560×1440 linear BGRA preview surfaces imported into EGL once.
Only the occupied rectangle is sampled; tile size changes do not reimport GPU
allocations. The larger 16-slot preview pool adds about 295 MiB of pixel
storage versus the former 1080p pool, before decoder/allocator overhead.
Detector, ReID and embedding tensor sizes are independent of source resolution.
GL fences protect preview surfaces from reuse while rendering.
Selection changes reuse these pools, including across camera reconnects.

Preview conversion runs separately from inference. AI keeps the latest decoded buffer per source. Visible presentation separately holds a three-frame FIFO, with two frame periods of initial prefill. A full FIFO replaces the oldest unconsumed frame; late frames are explicitly dropped to catch up. GPU/GL users retain their surfaces until completion. HIP packs RGB NCHW directly into the corresponding slice
of a reusable device batch arena; the shared batch executor then copies each slice
GPU-to-GPU into its own batch input. There is no host-to-device tensor copy. The scheduler rotates across eligible sources, waits at most
20 ms for a batch, uses batch 1 for a single image, and fills unused slots of a
larger batch with a HIP kernel. Only valid slots produce results. Postprocessing
is performed independently per image on the GPU. Allocator layout probes may
derive a newly-created, never-rendered test surface to discover pitches; they do not map or transfer frame pixels. The copy auditor
counts these separately and still rejects derivation of any rendered surface.
Each valid image reads back an 8404-byte compact detection structure (up to 300
boxes with their candidate index) as marked metadata; frame pixels remain on the GPU.

Detection/tracking admissions use `ai_fps` (0.1–10 Hz per camera, default 2). Camera groups are phased by compiled batch size; missed periods are skipped and queued work is replaced with newer inputs under overload. There is no adaptive rate, frame-age target or cross-model GPU lease. Actual completion rates can be lower than requested. Each result retains channel ID, generation and geometry. The standalone `amdpreproc` and `amdmigraphx` elements remain available and share the inference implementation.

## Verification

`ctest --test-dir build --output-on-failure` includes the existing hardware and
metadata tests and catalog tests for ownership, quota, active protection,
symlinks, path traversal and concurrent-writer exclusion.

`amdva-batch-reference` and `examples/nvr/tools/compare_batch_reference.py` compare distinct
images and valid batch occupancies 1–4 against CPU FP32 ONNX. Fixture upload and
framebuffer capture are explicitly offline test operations, outside the live
video processing path.

`amdva-nvr-rtsp-server H264_FILE H265_FILE [PORT]` serves 32 looped local camera
paths, alternating H.264/H.265. Use pre-encoded 1080p30 files with a starting
keyframe and no B frames; the test server does not encode or decode video.

For arbitrary MP4/MKV files and per-file camera URLs, use
[`amdva-rtsp-files`](../rtsp-files/README.md). It preserves file timing instead of assuming
1080p30 and accepts a text file list or positional paths.

For a 20-source configuration with the first 16 selected:

```bash
source scripts/env.sh
python3 examples/nvr/tools/nvr_soak.py /path/to/20-source-config.json \
  /path/to/1080p30.h264 /path/to/1080p30.h265 \
  --seconds=260 --cycles=100 --restart-at=100 \
  --output=artifacts/nvr-dynamic-validation
python3 examples/nvr/tools/nvr_soak.py /path/to/20-source-config.json \
  /path/to/1080p30.h264 /path/to/1080p30.h265 \
  --seconds=3600 --restart-at=600 --output=artifacts/nvr-soak
```

The harness verifies the view limit, repeated add/remove/select/unselect,
recovery after server restart, live copy audit, recording finalization and MKV
metadata. It captures an actual EGL-rendered tile image and reports per-channel
rates and GPU batch timing. Resource monitoring covers RSS, deduplicated DRM
allocation counters and FDs, with a 64 MiB growth guard for RSS/GPU allocation
and a bounded allowance for asynchronous recording finalizers. Telemetry also
reports live and free glibc heap bytes. NVR entry points initialize GStreamer with
a default limit of four glibc heap arenas, before worker creation; an explicit
`MALLOC_ARENA_MAX` environment setting takes precedence. Native callers should
call `initialize_runtime()` before creating Qt or GStreamer workers. After
pipelines are retired or reconnected, the controller
waits for frame owners to drain and trims free heap pages, with a five-second
cooldown, to limit RSS retained by transient GStreamer worker arenas. It aborts before
unbounded growth can exhaust host memory. Recorded validation results are in
`docs/dev/nvr/nvr-validation-results.json`.

On Ryzen AI 9 HX 370 / Radeon 890M, the one-hour local RTSP test passed with
20 recording sources and 16 selected views, 100 dynamic source/selection cycles,
and a server restart that swapped H.264/H.265 at the same URLs. All sources
recovered in seven seconds. Median per-view display and AI rates after recovery
were 29.88 and 5.98 FPS. All 1,304 finalized MKVs passed metadata validation;
60 frames from the latest recording of each of the 20 channels also decoded
successfully. Live copy audit reported zero violations. After warm-up, maximum
RSS and GPU allocation growth were 11,200 and 7,124 KiB, with no FD growth.

`amdva-nvr-faults CONFIG.json` uses the first source to exercise failed RTSP
registration, credential masking, 64×64 window resizing, recording-directory
permission failure and recovery. Use a dedicated recording directory; this test
temporarily changes its permissions.

The sample supports recording playback and timeline previews. It does not provide ONVIF discovery or audio recording, and makes no 4K performance claim. A production deployment additionally needs crash repair, access control, operational monitoring and camera-specific qualification. Historical validation figures below describe their recorded revisions, not current acceptance guarantees.

The framework's optional `keyframe-tests H264|H265 FILE OUTPUT_PREFIX` executable
(`AMDVA_HARDWARE_TESTS=ON`) checks
single-AU VA decoding and writes extracted AUs for offline reference work.
Use B-frame/open-GOP fixtures as well as closed-GOP fixtures to distinguish
ordinary intra pictures from valid random-access pictures. Extracted AUs and
`amdva-dump-tensor` readbacks are offline test artifacts, not live processing.
`examples/nvr/tools/compare_background_reference.py` compares live background boxes with CPU
FP32 ONNX detections from matching offline keyframe tensors.

The background reference comparator checks class/count agreement, IoU >= 0.95
and score delta <= 0.03. With `--allow-fp16-nms-ties`, it also accepts a
CPU-suppressed overlapping candidate whose score is within 0.005 of the CPU
winner, retaining those geometric/confidence checks. This addresses measured
FP16 ranking changes rather than lowering the IoU threshold; the report records
both the canonical post-NMS IoU and the number of alternate winners. On the
second bus IDR, one low-confidence box differs from the FP32 NMS winner (IoU
about 0.93), while matching the alternate CPU candidate above 0.994. The existing
offline GPU executor reproduces the background result on that same tensor.

Background AI completed the one-hour 20-source / 16-view test with 100 dynamic
cycles and codec-swapping reconnection, recovering receive/record/display in
7.5 seconds. All 1,309 MKVs passed metadata checks, and 60 frames from the latest
recording of every channel decoded successfully. Copy-audit violations were
zero; maximum RSS/GPU growth was 22.3/21.2 MiB, with at most four background jobs.
Sequential 70-second runs with background AI enabled/disabled measured display
medians of 29.86/29.90 FPS and selected AI medians of 5.98/5.98 FPS. These are
local-fixture observations. Full results and reference-comparison conditions
are in [background AI validation results](nvr-background-validation-results.json).

## Decode and display timing

The optional `telemetry` configuration defaults to `{ "enabled": true, "stall_ms": 1000 }`. Detailed camera statistics distinguish decoder-output notification, VA GPU-ready observation, preview, swap and actual presentation. The live tile shows `latency 10s max`, measured from first RTP reception to Wayland-confirmed presentation, and `N/A` when no valid confirmed measurement is available. It is not camera-exposure latency. Display drops after the shared ten-second startup exclusion are reported separately; low latency does not prove every frame was displayed.

The AI Statistics decode overview shows the largest per-camera input→GPU-ready P99 over ten seconds. GPU-ready observation includes polling/scheduling delay; it is not pure VCN execution time. Swap/decoder notifications cannot substitute for hardware completion or actual presentation. The headless service has no presentation measurement. Legacy SDK `decoded`/`display_fps` fields describe preview processing; use `Controller::telemetry()` for stage-level data and periodic JSON. See the [user guide](../../guide/nvr-demo.md#input-to-actual-display-latency-in-live-tiles), [telemetry](../telemetry.md) and [diagnostic investigation](input-decode-latency.md) for boundaries and strict validation.

## Person search

See [NVR semantic search](nvr-search.md) for native EmbeddingGemma 2 setup,
collection policy, text and region queries, and inline result playback.

## 1440p validation (2026-10-22)

The original sixteen 2560×1440, 30 FPS, 8-bit HEVC files in `~/work/videos/`
were served over RTSP without transcoding. A three-minute run exercised 16
visible cameras, four visible cameras and all cameras hidden in turn. All
inputs retained their original resolution; tracking settled at 1.98–2.02 Hz
per camera, with no tracking queue drops or camera errors. Process RSS was
about 6.3 GiB (peak 7.1 GiB). This is a short functional check, not a long soak.
The existing bounded YOLO, ReID and embedding input sizes were unchanged.

Text search, recording-region search, inline playback and deletion passed.
Full-size playback produced 2560×1440 output; 100 alternating H.264 1080p / HEVC
1440p hover previews passed with zero copy-audit or VPE errors (p95 89 ms).
At that revision, 1440p hover previews used a 428×240 GPU texture to respect VPE's 6:1
reduction limit, sampled into the former 320×180 popup by OpenGL. All 32 CTest tests
also passed.

The prepared local launcher is `bash artifacts/1440p-demo/run-demo.sh`.
It serves the original videos on port 18570 and uses isolated demo settings
and recordings under `artifacts/1440p-demo/` (these local artifacts are not
committed).
