# AI scheduling, statistics and 16-camera evaluation

The NVR has an **AI Statistics** tab with a 60-second rolling window. It shows
mean, P50, P95 and maximum completed wall time, average wait, calls per second,
logical batch size, pending work and cumulative drops. The summary combines
cameras; the detail selector exposes individual cameras. The camera table
shows detection/tracking update rates, confirmed people and tracking-frame age.

Detection/tracking can be changed from 0.1 to 10 Hz per camera in 0.1 Hz steps.
Embedding intervals can be changed from 0.1 to 60 seconds per person in
0.1-second steps. Press Enter or leave the field to apply the edit; changes take effect
immediately and are saved to the current configuration. Actual completion intervals and frame freshness are shown separately from the requested interval.

## Execution policy

Frequency and interval settings control admissions. There is no cross-model GPU
execution gate, priority aging, duty limit, automatic 20% EmbeddingGemma cooldown,
or adaptive extension of the requested interval. Each backend has one persistent worker and one active
job; each model retains its own serialization. Legacy valid
`mobileclip_gpu_budget_percent` / `mobileclip_npu_budget_percent` keys are accepted
with a deprecation warning, ignored and omitted when saving.

Preprocessing, batch assembly, MIGraphX inference, postprocessing and thumbnails
use independent nonblocking HIP streams. Completion waits apply only to the
owning stream or readiness event. Stream-ordered allocations retain their stream
owner until release. MIGraphX uses `run_async` followed by its context and owning
stream completion; callers still receive completed synchronous results.
The environment helper and native NVR initialization default `GPU_MAX_HW_QUEUES`
to 32 before HIP initialization, preserving explicit operator overrides. HIP
otherwise defaults to four hardware queues and can map unrelated streams to
the same queue; a stalled queue then blocks both. See [AMD HIP environment
variables](https://rocm.docs.amd.com/projects/HIP/en/docs-6.4.2/reference/env_variables.html).
The isolation contract holds a GPU semaphore on an unrelated stream and requires
inference to complete before releasing it.

ReID keeps VPE contexts across changing ROIs; the pinned Mesa implementation
recomputes coefficients when ratios change. Recreating these contexts for every
person had dominated preprocessing and stalled other work. ROI/batch parity is
checked against fresh-context features (cosine at least 0.9999). Thumbnails use
one lossless PNG encoding with libpng's fast flag and its documented output-size
bound, avoiding a full trial compression. The decoded pixels, preprocessing
namespace, model weights and database schema are unchanged; compressed PNGs can
be larger and continue to count against the recording quota.

Camera admissions are staggered in compiled-batch-sized groups in stable source
order across the configured period. Missed periods are skipped, without catch-up
bursts. Each camera uses its newest frame. Tracking holds at most 16 queued
camera frames, replacing older queued frames from the same camera. Embedding
holds the latest eligible crop per person up to `search.queue_limit` (default
128), expires departed people and never recomputes an unchanged image. The first
embedding is eligible immediately; later updates use the requested interval.
These are requested rates, subject to available hardware capacity.

`Embedding first result`, `Embedding update interval` (continuously eligible
people) and `Embedding freshness` expose actual delivery times. Deadline-miss
counters also include eligible requests that remain unserved; replacement and
queue-drop counters expose overload instead of silently changing settings.

The tracker retains its existing high/low confidence association and appearance
thresholds. Detection confidence 0.1 remains available for low-score recovery;
new identities still require 0.6 and confirmation. `search.min_person_height`
(default 0.1) excludes people shorter than that fraction of source-frame height
from crop/ReID/search processing. This is appropriate for the requested demo,
where distant small people need not be indexed.

YOLO preprocessing, inference, postprocessing and executor total are separate
rows. **Do not sum overlapping rows**: executor total includes its components,
and detection updates describe scheduling, not extra inference work. A ReID
call may process multiple physical batches; its mean batch column is the number
of real people in that frame. Queue waits include intentional pacing;
per-camera detection/tracking wait values represent frame age. Drop counters
count rejected/replaced work items, not distinct people: one person can have
many candidates skipped while the queue is full. Model loading
and warmup are excluded. These are wall-clock measurements, not GPU event
utilization counters. Samples and row counts are bounded.

## NPU embedding timing

`Image embedding NPU` and the NPU overview show the inference call after GPU
preprocessing has completed. They
include the GStreamer/ORT call and compact result handling. They exclude GPU
input preprocessing.

| Row | Execution time | Wait column |
| --- | --- | --- |
| `Image embedding NPU preprocess` | HIP input preparation | Dispatch overhead (no application execution gate) |
| `Image embedding NPU` | NPU inference call | 0 |
| `Image embedding NPU total` | Preparation + inference | 0 |

The generic `Image embedding` row still measures the entire device job. Do not
sum these overlapping rows. No artificial cooldown is applied.

Before removing the execution gate, on the 17-source demo a measured 311.27 ms total consisted of 297.85 ms waiting
for the GPU, 0.53 ms preparation and 12.89 ms inference. ORT profiling assigned
all 13 image runs to `VitisAIExecutionProvider` (mean node duration 12.42 ms),
with no CPU EP nodes. The earlier NPU row included GPU admission wait; its
roughly 300 ms value did not represent NPU compute time. See the
[recorded timing investigation](../npu-timing-results.json).

The runtime disables CPU EP fallback. `Ort::MemoryInfo::CreateCpu` describes the
shared host input allocation, not the execution provider. To collect an optional
image-session ORT profile, set `AMDVA_NPU_PROFILE_PREFIX` to a file prefix in an
existing directory before launch. Files include PID/session identifiers and
are flushed when the image session is released. Text sessions are not profiled.

```bash
mkdir -p artifacts/npu-profile
AMDVA_NPU_PROFILE_PREFIX="$PWD/artifacts/npu-profile/image" \
  examples/nvr/scripts/run.sh /path/to/nvr-config.json
```

This verifies ORT provider assignment; it does not inspect Vitis AI internals.

## Selected configuration

| Stage | Model/input, width × height | Batch | Reason |
|---|---|---:|---|
| Detection | YOLO11n FP16, 640 × 384 | 1/4 | Better high-confidence recall on reviewed demo frames than YOLOv8n; reduced rectangular input |
| ReID | OSNet x0.25 MSMT17 FP16, 128 × 256 | 4 | Lower inference cost while retaining full person height |
| Semantic image | EmbeddingGemma 2 native ROCm, 108 × 192, token budget 70 | up to 4 | Portrait input and bounded blocking time |

The runtime supports YOLO rectangular dimensions up to 640 on each axis and
ReID batches 1–16 with height 64–256 and width 32–128. It validates the compiled
model's tensor contract. YOLO11 uses the existing YOLOv8-family output decoder.
YOLOX Nano was benchmarked separately with its own decoder; it was not selected
and is not accepted as an interchangeable live-model output format.

Recommended search settings:

```json
{
  "ai_fps": 2,
  "search": {
    "image_size": 256,
    "interval_ms": 5000,
    "queue_limit": 128,
    "embedding_width": 108,
    "embedding_height": 192,
    "embedding_batch": 4,
    "min_person_height": 0.1
  }
}
```

`examples/nvr/config/nvr-ai-16.json` provides a complete 16-source example and uses
`examples/nvr/config/yolo11n-rect.json` for the rectangular detector metadata. Prepare the
referenced models and native bundle before launching it.

These fields supplement the library, bundle and model paths in the normal NVR
configuration. `image_size` bounds the stored RGB crop; ReID now preprocesses the original
VA frame ROI through `amdpreproc`, independently of this limit. Embedding
gets a separate smaller, aspect-preserving input with black padding and no
upscaling. Both embedding dimensions must be set together. Leaving both zero
preserves the legacy square preprocessing namespace. Collection
intervals down to 100 ms are honored as requested targets for every embedding backend.
Changing preprocessing or ReID weights selects a different search namespace;
old observations remain in the catalog and can be queried with their original
configuration. There is no automatic backfill.

Prepare portrait native profiles for batches 1, 2, 4 and 8 using the pinned
`embeddinggemma2-rocm` source and matching ROCm/LibTorch described in
[nvr-search.md](nvr-search.md). Keep text profiles in the bundle. For example:

```bash
eg2 prepare --model /path/to/google --output artifacts/embedding-portrait \
  --budgets 70 --batches 1 2 4 8 --aspect 9 16 --precision fast
```

`examples/nvr/scripts/merge_embedding_profiles.py` combines compatible prepared bundles
using local symlinks. The runtime pads partial image batches by repeating the last valid image and
discards the extra outputs, including for region queries. This keeps one fixed
physical image profile resident: the native library otherwise reloads its
image profile when switching batch shapes. That profile is warmed before
cameras start. Text inference uses a separate profile cache.

## Reproduce measurements

The native benchmark and validation executables require a build configured
with `AMDVA_HARDWARE_TESTS=ON`. Run GPU tests sequentially, with the NVR stopped.
Record the machine's power
settings along with results; timings taken before a power-setting change must
not be used as the final comparison.

```bash
source scripts/env.sh
.deps/model-export/bin/python examples/nvr/scripts/benchmark_ai.py --screen
# Select finalists, then rerun cached models with 20 warmups and
# at least 200 iterations AND 30 seconds, repeated three times:
.deps/model-export/bin/python examples/nvr/scripts/benchmark_ai.py \
  --models yolo11n --sizes 640x384 --batches 1 4 --remeasure
.deps/model-export/bin/python examples/nvr/scripts/benchmark_ai.py \
  --models osnet_x0_25 --sizes 128x256 --batches 1 4 --remeasure
python3 examples/nvr/scripts/benchmark_embedding.py --library /path/to/libeg2.so \
  --bundle /path/to/prepared-bundle --sizes 108x192 --batches 4
```

The detector/ReID benchmark measures model execution only with a resident
input. Native embedding includes preprocessing and output transfer. Full-pipeline
statistics include crop extraction, preprocessing and queueing separately.
ReID preprocessing now measures the `amdpreproc` VPE/HIP path rather than CPU resizing.
`--seconds 3 --repeats 1` is useful for exploratory screening but is not a final
performance measurement. Compiled artifacts and successful results are cached;
new outputs are renamed atomically so an interrupted run can resume.

`examples/nvr/benchmarks/person-review.json` contains 53 visually reviewed boxes from 16
frames. `examples/nvr/benchmarks/appearance-review.json` contains 52 reviewed person crops.
Create independent 1080p copies and quality fixtures from `~/work/videos`:

```bash
.deps/model-export/bin/python examples/nvr/scripts/prepare_ai_fixture.py
```

The user reports 40 people in total across the original sixteen 10-minute
videos. The short load-test excerpts do not establish identity continuity over
the full ten minutes; loop cuts and camera-local IDs can increase the ID count.
The original videos are not modified. The quality scripts compare detector
recall at confidence 0.1/0.5/0.6, ReID retrieval and native FP16 parity, and text
retrieval across embedding input sizes. These are small demo-specific checks,
not exhaustive annotations or a temporal tracking IDF1 benchmark. Formal
IDF1 and a worst-case 16 × 8 simultaneous-person load remain separate validation
requirements. Reducing embedding pixels does not proportionally reduce model
execution because the token budget and native internal profile still matter.

For integration, serve the fixture list and use a dedicated recording root:

```bash
build/examples/rtsp-files/amdva-rtsp-files \
  --list artifacts/ai-benchmark/videos.txt --port 18569
# In another terminal with the native runtime environment configured:
build/examples/nvr/amdva-ai-validate examples/nvr/config/nvr-ai-16.json 1800 \
  artifacts/ai-benchmark/soak.jsonl
```

The validator tests frequency controls and saved settings, runs all-visible,
four-visible and all-hidden camera phases, records five-second JSON snapshots,
and captures the statistics screen at the end. It checks that every camera
continues inference and that semantic observations are saved. Inspect stable
windows of each phase for rate, queue and memory behavior; startup is included
in the log. Use `amdva-search-validate` with a separate two-camera fixture to
check text search, person grouping, region search, inline playback and deletion.

## Measured results on this host

Final isolated measurements on 2026-10-22, after the user reduced the machine's
power setting, used Ryzen AI 9 HX 370 / Radeon 890M (gfx1150), kernel
6.17.0-1033-oem. The exact BIOS power limit was not recorded. Three repeats
produced these average batch times:

| Model/input | Batch | Batch ms | Per image ms |
|---|---:|---:|---:|
| YOLOv8n 640 × 640 baseline | 4 | 9.705 | 2.426 |
| YOLO11n 640 × 384 | 1 | 1.553 | 1.553 |
| YOLO11n 640 × 384 | 4 | 5.549 | 1.387 |
| OSNet x1.0 128 × 256 baseline | 1 | 1.400 | 1.400 |
| OSNet x0.25 128 × 256 | 1 | 0.690 | 0.690 |
| OSNet x0.25 128 × 256 | 4 | 1.206 | 0.302 |
| Embedding 384 × 384 baseline | 1 | 52.394 | 52.394 |
| Embedding 108 × 192 | 4 | 195.666 | 48.917 |

Embedding reduction is only about 7% per image here. Its approximately 196 ms
non-preemptible batch remains the largest inference section, so frequency and
pacing matter more than pixel reduction. At that isolated cost, 128 active
people would have required about 31 seconds between embeddings under the former 20% duty
budget. That automatic throttle has been removed. This historical calculation
is an estimate, not a measured 128-person throughput result.

On the reviewed 53-box subset, YOLO11n 640 × 384 matched 52 boxes at confidence
0.1, 51 at 0.5 and 49 at 0.6. The YOLOv8n 640-square baseline matched 53/45/42;
YOLOX Nano matched 45/38/33 and was rejected. Native FP16 YOLO11 reproduced the
52/51/49 counts. Native ReID rank-1 on 52 reviewed crops was 84.6% for selected
OSNet x0.25 versus 82.7% for x1.0. At 64 × 128 the x0.25 result fell to 55.8%,
so the full-height input was retained. With 11 clothing queries, mean semantic
Recall@5 was 82.6% for 108 × 192 versus 66.7% for 384-square; each shape had at
least one relevant hit in the first five results for every query. These small
fixtures support the demo selection but do not establish general accuracy.

Raw repeat statistics and compact quality results are checked in as
[final timings](../../../examples/nvr/benchmarks/ai-final-2026-10-22.json) and
[quality checks](../../../examples/nvr/benchmarks/ai-quality-2026-10-22.json). The broader initial
matrix used short exploratory runs, mostly batches 1 and 4. Detector/ReID
batch-8/16 compilation was interrupted by host restarts and is not represented
as a completed comparison. Batch 4 is the validated live configuration.

## 16-camera integration result

The 1,804-second run completed with ten-minute all-visible, four-visible and
all-hidden phases, checked against actual camera selection and slot state.
In stable windows, every camera's detection and tracking rates stayed between
1.983 and 2.017 Hz. All sixteen cameras produced stored observations. There
were 5,401 cumulative saves; five-minute retention intentionally removes older
rows. Resident memory in stable windows ranged from 6,300 to 6,318 MiB; peak
RSS, including startup, was 7,381 MiB. No camera/search errors or host restarts
occurred in this run. The embedding queue stayed at or below 128; candidate
skips occurred under load and the effective interval was generally 30 seconds.
This does not establish five-second coverage for every person.

The final screenshot exposed an unset decoded-frame timestamp in continuous
hidden-camera analysis. That affected the displayed frame age/wait, not model
execution durations or update counts. The timestamp is now set on decoded
frames, and the validator explicitly rejects stale tracking timestamps. The
long-run report omits the invalid frame-age wait fields for mixed/hidden phases;
a subsequent 120-second validation passed in all three modes, with tracking
ages between 71 and 726 ms. Tracking queue drops were zero throughout the
30-minute run.

See [integration results](../../../examples/nvr/benchmarks/ai-soak-2026-10-22.json). The source
fixture is sixteen 30-second 1080p excerpts, not the full ten-minute originals.
Neither this throughput run nor the reviewed still-image subsets establishes
IDF1 or that all 40 physical people retain one identity throughout the source
videos. Tracking remains camera-local.

## Frequency-only validation

`examples/nvr/config/nvr-frequency-17.json` provides the fixed-load configuration.
Set `AMDVA_RYZEN_AI_VENV` to the prepared Ryzen AI environment when launching it.
Its input servers use ports 18570 (16 video files) and 18572 (dashboard).
Run `amdva-scheduling-validate CONFIG [SECONDS=120]` with
`libamdva_copy_audit.so` preloaded after starting the configured RTSP fixtures.
It warms up for 15 seconds and measures the fixed 17-source demo with 16 selected
sources, detection/tracking at 4 Hz and embedding interval 3000 ms. Preview is
disabled for reproducible backend measurements. Settings are never lowered by
the verifier. It reports per-camera detection rates, tracking completions against
all eligible submissions, worst observed rolling-window P95 and unserved deadline
misses. For the backend verifier, initialize Ryzen AI before running the binary:

```bash
source scripts/env.sh
source examples/nvr/scripts/mobileclip-env.sh "$AMDVA_RYZEN_AI_VENV"
LD_PRELOAD="$PWD/build/src/gst/libamdva_copy_audit.so" \
  build/examples/nvr/amdva-scheduling-validate examples/nvr/config/nvr-frequency-17.json
```

Targets are detection at least 90% of configured frequency, tracking at
least 90% of eligible frames, NPU preparation through inference P95 at most 50 ms,
first embedding within 1 second and continuous update interval P95 at most 1.2
times the requested interval. Missed unserved requests also fail the check.

### Measured result and remaining limit

The [recorded frequency-only validation](frequency-scheduling-results.json) uses
17 inputs / 16 selected, 4 Hz and 3000 ms without lowering the settings. Over
120.634 seconds after warmup, tracking completed 8134/8134 eligible frames with
no queued-frame drops. NPU preparation through inference had a worst observed
rolling P95 of 19.69 ms; application input copies and audit violations were zero.

**The full deadline acceptance check did not pass.** Eight first-result deadlines
and six update deadlines were missed. The maximum first-result delay was
3074 ms, maximum continuous update interval 5083 ms, and one NPU job took about
2098 ms despite the low P95. During the run, the kernel logged
`amdxdna ... aie2_tdr_detect: TDR timeout detected` at 22:52:07 JST, context 10.
An earlier run also returned `ERT_CMD_STATE_TIMEOUT`. Persistent backend workers
do not eliminate these outliers. These observations locate a remaining problem
at the NPU execution boundary; provider/firmware internals and a permanent fix
were outside this change. No fallback, hidden frequency reduction, retry that
conceals a failed sample, or relaxed acceptance threshold was introduced.

The 30-second check after VPE-context reuse passed all targets, but does not
replace the longer failing measurement. The reference disables preview; separate
GUI checks cover rendering and search behavior, not the same 17-source latency
claim. Compressed thumbnail storage may increase with fast lossless PNG encoding.

## ReID readiness and counters

Person tracking has its own `tracking_state` / `tracking_error`, independent of
embedding/search `state` / `error`. An embedding model load or warmup failure
still starts the ReID worker after the attempted HIP graph capture has ended.
Live detection submits tracking frames whenever tracking is ready. A later
embedding failure stops that backend and clears its pending crops; confirmed
person IDs and tracking updates continue. The provider error remains visible,
and embedding queries fail explicitly without a CPU/GPU fallback.

The detailed AI table distinguishes **Samples (60s)** (the bounded rolling
window, which can stay constant at steady load) from **Completed (total)**
(cumulative completed stage measurements for this process). Use the latter to
verify that ReID continues to execute. Tracking readiness/errors are shown
separately from the embedding state.

`reid-isolation-contract` injects embedding load, warmup and live-inference
failures and requires six real GPU ReID updates with a stable confirmed ID in
each case. Enable it with `AMDVA_HARDWARE_TESTS=ON`, `AMDVA_TEST_VIDEO` and
`AMDVA_REID_TEST_MODEL` pointing to prepared H.264/OSNet fixtures. The fault
library is test-only. `amdva-reid-demo-validate CONFIG SECONDS OUTPUT.jsonl`
checks live ReID progress, fresh confirmed people and the cumulative count in
the actual GUI while retaining embedding errors in its log. This validator
checks ReID availability, not embedding success. See the
[ReID isolation results](reid-isolation-results.json).
