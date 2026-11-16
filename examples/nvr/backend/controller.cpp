#include "backend/controller.hpp"
#include "backend/preview_publication.hpp"
#include "backend/encoded_dispatch.hpp"
#include "backend/recording_clock.hpp"
#include "amdva/batching.hpp"
#include "amdva/diagnostics.hpp"
#include "amdva/copy_audit.hpp"
#include "amdva/vpe_audit.hpp"
#include <fstream>
#include "amdva/preprocess.hpp"
#include "amdva/streaming/ingest.hpp"
#include "amdva/streaming/keyframe.hpp"
#include "amdva/streaming/playback.hpp"
#include "amdva/streaming/recorder.hpp"
#include "backend/config.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/va/gstva.h>
#include <gst/va/gstvaallocator.h>
#include <gst/va/gstvadisplay_drm.h>
#include <gst/va/gstvautils.h>
#include <iostream>
#include <malloc.h>
#include <map>
#include <random>
namespace amdva::nvr {
using namespace amdva::streaming;
namespace {
// Control-path spans above 1 ms identify where ingest draining is delayed.
struct DiagnosticControlSpan {
    const std::string source;
    const char *event;
    int64_t start;
    DiagnosticControlSpan(std::string source, const char *event)
        : source(std::move(source)), event(event),
          start(diagnostics_enabled() ? Telemetry::monotonic_ns() : 0) {}
    ~DiagnosticControlSpan() {
        if (!start) return;
        const auto end=Telemetry::monotonic_ns();
        if (end-start>=1000000)
            diagnostic_event(source,0,0,event,end,end-start);
    }
};
constexpr uint64_t compressed_limit = 16ull << 20;
GstElement *parse(const std::string &description) {
    GError *error{};
    auto *p = gst_parse_launch(description.c_str(), &error);
    if (error) {
        std::string text = error->message;
        g_clear_error(&error);
        if (p)
            gst_object_unref(p);
        throw std::runtime_error(text);
    }
    if (!p)
        throw std::runtime_error("Cannot create pipeline");
    return p;
}
void destroy(GstElement *&p) {
    if (p) {
        gst_element_set_state(p, GST_STATE_NULL);
        gst_object_unref(p);
        p = nullptr;
    }
}
Sample own(GstSample *s) {
    return Sample(s, [](GstSample *p) { gst_sample_unref(p); });
}
std::string identifier() {
    gchar *p = g_uuid_string_random();
    std::string s = p;
    g_free(p);
    return s;
}
std::string message_error(GstMessage *m) {
    GError *e{};
    gchar *debug{};
    gst_message_parse_error(m, &e, &debug);
    std::string s = e ? e->message : "Pipeline error";
    g_clear_error(&e);
    g_free(debug);
    return s;
}
// Never return raw plugin messages to the UI: rtspsrc errors may include credentials.
std::string public_error(const std::string &error, const Source &source) {
    if (error.find(source.uri) != std::string::npos || error.find("rtsp://") != std::string::npos ||
        error.find("rtsps://") != std::string::npos)
        return "RTSP operation failed";
    return error;
}
} // namespace
Slot::~Slot() {
    // The base releases the preview surfaces and the VA session afterwards.
    image.reset();
    if (va)
        va->destroy_surface(ai_surface);
}
namespace {
void retire_presentation(std::vector<PresentationFifo<Frame>::Entry> entries,
                         const char *event,int reason=0) {
    for(const auto &entry:entries)
        if(entry.frame.timing.telemetry)
            entry.frame.timing.telemetry->diagnostic(entry.frame.timing.frame,event,reason);
}
}
struct Controller::Channel {
    Controller &controller;
    uint64_t decoded_serial{};
    bool pool_reported{};
    Status state;
    std::shared_ptr<Telemetry> timing;
    std::unique_ptr<PipelineTelemetry> receive_timing, decode_timing;
    std::unique_ptr<RtspIngest> ingest;
    GstElement *decoder{}, *decode_input{};
    RecordingClock recording_clock;
    std::atomic<bool> dispatch_closing{false};
    EncodedDispatch<Sample> input_dispatch{
        compressed_limit,
        [](const Sample &s) { return gst_buffer_get_size(gst_sample_get_buffer(s.get())); },
        [](const Sample &s) { return !GST_BUFFER_FLAG_IS_SET(gst_sample_get_buffer(s.get()), GST_BUFFER_FLAG_DELTA_UNIT); },
        [](const Sample &s) { return ingest_sequence(s); },
        [this](const Sample &s) {
            const auto trace=frame_trace(gst_sample_get_buffer(s.get()));
            if(trace.telemetry) trace.telemetry->diagnostic(trace.frame,"decode_preroll");
        }};
    void retire_dispatch() {
        for (const auto &sample:input_dispatch.close()) {
            const auto trace=frame_trace(gst_sample_get_buffer(sample.get()));
            if(trace.telemetry) trace.telemetry->diagnostic(trace.frame,"decode_dispatch_retired");
        }
    }
    void update_dispatch_mode() {
        if (state.slot>=0 || state.source.transcode.enabled ||
            (controller.options_.background_ai_enabled && continuous_search())) input_dispatch.enable();
        else retire_dispatch();
    }
    void publish_recording_clock(GstSample *sample) {
        if (!recording) return;
        const auto pts=GST_BUFFER_PTS(gst_sample_get_buffer(sample));
        const auto wall=recording->wall_time_ms(pts);
        if(wall) recording_clock.publish(pts,*wall,recording->session());
    }
    void activate_dispatch(bool transcoded, const Sample &seed) {
        input_dispatch.activate([this,transcoded](const Sample &sample) {
            if (transcoded) {
                auto *buffer=gst_sample_get_buffer(sample.get());
                const auto pts=GST_BUFFER_PTS(buffer);
                stamp(pts,pts);
                {
                    std::lock_guard<std::mutex> lock(transcode_wall_mutex);
                    transcode_wall[pts]=g_get_real_time()/1000;
                    while(transcode_wall.size()>512) transcode_wall.erase(transcode_wall.begin());
                }
                if(!transcoder->push(sample.get())) throw std::runtime_error("Transcoder input backlog or invalid PTS");
            } else if(!push(decode_input,sample,decode_origin)) {
                throw std::runtime_error("Decoder input backlog or invalid PTS");
            }
        },seed);
    }
    std::vector<std::unique_ptr<SegmentRecorder>> recordings;
    SegmentRecorder *recording{};
    // With Source::transcode, received samples go through the transcoder and the
    // recorder opens on its first output. transcode_wall keeps the receive time of each
    // pushed PTS so the recording clock does not include the encode delay.
    std::unique_ptr<Transcoder> transcoder;
    std::mutex transcode_wall_mutex;
    std::map<GstClockTime, int64_t> transcode_wall;
    uint64_t output_bytes{}, output_bytes_base{}, output_frames{}, output_frames_base{};
    // 0 until the stream is seen, 1 H.264, 2 H.265, -1 unsupported.
    int codec_id() const { return ingest ? ingest->codec() : 0; }
    uint64_t input_frames_base{}, input_bytes_base{};
    Clock::time_point input_rates_at{Clock::now()};
    void reset_input_metrics() {
        input_frames_base = 0;
        input_bytes_base = 0;
        input_rates_at = Clock::now();
        state.input_codec.clear();
        state.input_width = state.input_height = 0;
        state.input_fps = state.input_mbps = 0;
        reset_output_metrics();
    }
    void reset_output_metrics() {
        output_bytes = output_bytes_base = output_frames = output_frames_base = 0;
        state.output_codec.clear();
        state.output_width = state.output_height = 0;
        state.output_fps = state.output_mbps = state.encode_ms = 0;
    }
    void update_input_rates() {
        const auto now = Clock::now();
        const double seconds = std::chrono::duration<double>(now - input_rates_at).count();
        if (seconds < 1)
            return;
        const auto frames = ingest ? ingest->frames() : 0, bytes = ingest ? ingest->bytes() : 0;
        state.input_fps = (frames - input_frames_base) / seconds;
        state.input_mbps = (bytes - input_bytes_base) * 8.0 / seconds / 1000000.0;
        state.output_mbps = (output_bytes - output_bytes_base) * 8.0 / seconds / 1000000.0;
        state.output_fps = (output_frames - output_frames_base) / seconds;
        state.encode_ms = transcoder ? transcoder->latency_ms() : 0;
        input_frames_base = frames;
        input_bytes_base = bytes;
        output_bytes_base = output_bytes;
        output_frames_base = output_frames;
        input_rates_at = now;
    }
    void input_format(GstCaps *caps) {
        if (!caps || gst_caps_is_empty(caps))
            return;
        const auto *format = gst_caps_get_structure(caps, 0);
        const char *name = gst_structure_get_name(format);
        state.input_codec = g_str_equal(name, "video/x-h264")   ? "H.264"
                            : g_str_equal(name, "video/x-h265") ? "H.265"
                                                                : "Unknown";
        int width = 0, height = 0;
        gst_structure_get_int(format, "width", &width);
        gst_structure_get_int(format, "height", &height);
        state.input_width = unsigned(std::max(0, width));
        state.input_height = unsigned(std::max(0, height));
    }
    std::atomic<bool> record_failed{false};
    bool removed{}, decode_wait{true};
    GstClockTime decode_origin{GST_CLOCK_TIME_NONE};
    struct SearchStamp {
        int64_t wall_ms{};
        std::string session;
    };
    std::mutex search_stamp_mutex;
    std::map<GstClockTime, SearchStamp> search_stamps;
    void stamp(GstClockTime decoded_pts, GstClockTime original_pts) {
        const auto mapped = recording_clock.at(original_pts);
        if (!mapped) return;
        std::lock_guard<std::mutex> lock(search_stamp_mutex);
        search_stamps[decoded_pts] = {mapped->wall_ms, mapped->session};
        while (search_stamps.size() > 256)
            search_stamps.erase(search_stamps.begin());
    }
    uint64_t logged_display_drops{};
    Clock::time_point display_drop_log_at{};
    unsigned retries{};
    Clock::time_point reconnect{Clock::now()}, record_retry{};
    uint64_t decode_count_base{}, ai_count_base{};
    KeyframeCache keyframes;
    int background_slot{-1};
    bool background_counted{};
    Clock::time_point background_due{}, background_deadline{}, background_captured{},
        background_result_at{};
    int64_t background_capture_us{};
    Channel(Controller &c, Source source) : controller(c) {
        state.source = std::move(source);
        timing = std::make_shared<Telemetry>(state.source.id, controller.options_.telemetry);
        std::lock_guard<std::mutex> lock(controller.telemetry_mutex_);
        timing->presentation_drop_count_after(controller.presentation_drop_count_after_);
        controller.telemetry_[state.source.id] = timing;
        state.source.selected = false;
    }
    ~Channel() {
        stop_receive();
        stop_decode();
        recordings.clear();
    }
    void reset_presentation() {
        if(state.slot<0)return;
        auto &slot=controller.slots_[state.slot];
        retire_presentation(slot.presentation.reset(state.generation,
            controller.options_.preview_enabled && controller.presentation_observed_.load() &&
            slot.preview_visible.load()),"preview_not_requested",8);
        pool_reported=false;
    }
    static GstFlowReturn decoded(GstAppSink *sink, gpointer data) {
        auto &c = *static_cast<Channel *>(data);
        auto s = own(gst_app_sink_pull_sample(sink));
        if (!s)
            return GST_FLOW_EOS;
        GstVideoInfo info{};
        if (!gst_video_info_from_caps(&info, gst_sample_get_caps(s.get())) ||
            GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12 ||
            info.width > int(max_video_width) || info.height > int(max_video_height) ||
            info.width % 2 || info.height % 2 ||
            info.interlace_mode != GST_VIDEO_INTERLACE_MODE_PROGRESSIVE ||
            (info.colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_UNKNOWN &&
             info.colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_BT709) ||
            (info.colorimetry.range != GST_VIDEO_COLOR_RANGE_UNKNOWN &&
             info.colorimetry.range != GST_VIDEO_COLOR_RANGE_16_235))
            return GST_FLOW_NOT_NEGOTIATED;
        // slot and generation cannot change until set_state(NULL) has joined this callback.
        const int index = c.state.slot >= 0 ? c.state.slot : c.background_slot;
        if (index < 0)
            return GST_FLOW_OK;
        auto *crop = gst_buffer_get_video_crop_meta(gst_sample_get_buffer(s.get()));
        if (crop && (crop->x || crop->y || crop->width != unsigned(info.width) ||
                     crop->height != unsigned(info.height)))
            return GST_FLOW_NOT_NEGOTIATED;
        auto &slot = c.controller.ai_slot(index);
        const auto lock_start = Telemetry::monotonic_ns();
        std::unique_lock<std::mutex> l(slot.mutex);
        const auto acquired = Telemetry::monotonic_ns();
        const auto diagnostic_trace = frame_trace(gst_sample_get_buffer(s.get()));
        c.timing->diagnostic(diagnostic_trace.frame, "decoded_lock", acquired-lock_start, 0,
                             GST_BUFFER_PTS(gst_sample_get_buffer(s.get())), acquired);
        if (slot.latest.sample && (slot.latest.width != unsigned(info.width) ||
                                   slot.latest.height != unsigned(info.height)))
            slot.detections.clear();
        if (index>=16 && slot.latest.sample && !slot.latest.preview_consumed) {
            c.timing->diagnostic(slot.latest.timing.frame,"latest_replaced");
            c.timing->counter(TimingCounter::LatestReplaced);
        }
        auto trace = frame_trace(gst_sample_get_buffer(s.get()));
        if(!trace.telemetry)trace.telemetry=c.timing; // Always account live display loss.
        slot.latest = {std::move(trace),
                       std::move(s),
                       c.state.generation,
                       unsigned(info.width),
                       unsigned(info.height),
                       index >= 16 && !c.continuous_search() ? c.background_captured
                                                             : Clock::now()};
        {
            std::lock_guard<std::mutex> lock(c.search_stamp_mutex);
            auto it = c.search_stamps.find(
                GST_BUFFER_PTS(gst_sample_get_buffer(slot.latest.sample.get())));
            if (it != c.search_stamps.end()) {
                slot.latest.wall_time_ms = it->second.wall_ms;
                slot.latest.recording_session = it->second.session;
                c.search_stamps.erase(it);
            }
        }
        auto presentation_frame=slot.latest;
        // AI retains the newest reference independently. Presentation owns each
        // admitted frame in FIFO order; replacing the AI pointer is not a drop.
        if(index<16)slot.latest.preview_consumed=true;
        c.timing->diagnostic(diagnostic_trace.frame, "handoff", Telemetry::monotonic_ns()-acquired);
        l.unlock();
        if(index<16) {
            if(!c.pool_reported) {
                auto *element=c.transcoder?GST_ELEMENT(gst_object_ref(c.transcoder->decoder())):
                    gst_bin_get_by_name(GST_BIN(c.decoder),"decode");
                auto *pool=gst_video_decoder_get_buffer_pool(GST_VIDEO_DECODER(element));
                if(pool) {
                    auto *config=gst_buffer_pool_get_config(pool);
                    guint size=0,minimum=0,maximum=0;GstCaps *caps=nullptr;
                    gst_buffer_pool_config_get_params(config,&caps,&size,&minimum,&maximum);
                    c.timing->diagnostic(diagnostic_trace.frame,"decoder_pool",minimum,maximum);
                    c.timing->diagnostic(diagnostic_trace.frame,"decoder_pool_bytes",size,4);
                    gst_structure_free(config);gst_object_unref(pool);
                }
                gst_object_unref(element);c.pool_reported=true;
            }
            const auto serial=++c.decoded_serial;
            const auto presentation_pts=GST_BUFFER_PTS(gst_sample_get_buffer(presentation_frame.sample.get()));
            const auto period=info.fps_n>0 && info.fps_d>0 ?
                int64_t(GST_SECOND)*info.fps_d/info.fps_n:0;
            const auto before=Telemetry::monotonic_ns();
            try {
                auto result=PresentationFifo<Frame>::Result::Inactive;
                std::vector<PresentationFifo<Frame>::Dropped> dropped;
                if(c.controller.options_.preview_enabled &&
                   c.controller.presentation_observed_.load() && slot.preview_visible.load())
                    result=slot.presentation.push(std::move(presentation_frame),c.state.generation,
                        serial,presentation_pts,period,&dropped);
                for(const auto &discarded:dropped) {
                    const auto &trace=discarded.entry.frame.timing;
                    if(trace.telemetry) {
                        trace.telemetry->counter(TimingCounter::PresentationDropped);
                        trace.telemetry->diagnostic(trace.frame,"presentation_fifo_dropped",
                            int64_t(discarded.reason),discarded.entry.serial);
                    }
                }
                const auto snapshot=slot.presentation.snapshot();
                if(result==PresentationFifo<Frame>::Result::Queued)
                    c.timing->diagnostic(diagnostic_trace.frame,"fifo_deadline",snapshot.next_due_ns,snapshot.clock_resyncs);
                if(result==PresentationFifo<Frame>::Result::Queued) {
                    c.timing->diagnostic(diagnostic_trace.frame,"fifo_enter",snapshot.frames,period);
                    c.timing->diagnostic(diagnostic_trace.frame,"fifo_wait",Telemetry::monotonic_ns()-before);
                    c.timing->diagnostic(diagnostic_trace.frame,"fifo_oldest",snapshot.oldest_ns);
                } else {
                    c.timing->diagnostic(diagnostic_trace.frame,
                        c.controller.stopping_.load()?"shutdown_fifo_retired":"preview_not_requested",
                        result==PresentationFifo<Frame>::Result::Retired?8:0);
                }
            } catch(const std::exception &e) {
                c.timing->diagnostic(diagnostic_trace.frame,"presentation_fifo_error",1);
                std::lock_guard<std::mutex> lock(slot.mutex);slot.error=e.what();
                return GST_FLOW_ERROR;
            }
        }
        return GST_FLOW_OK;
    }
    void start_receive() {
        try {
            reset_input_metrics();
            keyframes.reset();
            state.connection = "connecting";
            state.error.clear();
            state.generation = ++controller.next_generation_;
            if (state.slot >= 0) {
                auto &slot = controller.slots_[state.slot];
                std::lock_guard<std::mutex> l(slot.mutex);
                slot.generation = state.generation;
                slot.latest = {};
                slot.detections.clear();
            }
            timing->begin_generation(state.generation);
            receive_timing = std::make_unique<PipelineTelemetry>(timing);
            receive_timing->background(state.slot < 0);
            RtspIngest::Options options;
            options.uri = state.source.uri;
            options.queue_limit_bytes = compressed_limit;
            RtspIngest::Hooks hooks;
            hooks.on_source = [this](GstElement *src) { receive_timing->attach_rtsp(src); };
            hooks.on_access_unit_pad = [this](GstPad *pad) {
                receive_timing->attach_access_units(pad);
            };
            hooks.on_discarded = [this](uint64_t count) {
                timing->counter(TimingCounter::EncodedDiscarded, count);
            };
            update_dispatch_mode();
            hooks.on_sample = [this](const Sample &sample) {
                const auto started=diagnostics_enabled()?Telemetry::monotonic_ns():0;
                const bool accepted=input_dispatch.receive(sample);
                if(!accepted && dispatch_closing.load()) {
                    const auto trace=frame_trace(gst_sample_get_buffer(sample.get()));
                    if(trace.telemetry) trace.telemetry->diagnostic(trace.frame,"decode_dispatch_retired");
                }
                if(started) {
                    const auto trace=frame_trace(gst_sample_get_buffer(sample.get()));
                    const auto queue=input_dispatch.snapshot();
                    timing->diagnostic(trace.frame,"decode_dispatch_wait",Telemetry::monotonic_ns()-started);
                    timing->diagnostic(trace.frame,"decode_dispatch_queue",queue.bytes,queue.frames);
                    timing->diagnostic(trace.frame,"decode_dispatch_oldest",queue.oldest_ns);
                }
            };
            ingest = std::make_unique<RtspIngest>(std::move(options), std::move(hooks));
            ingest->start();
        } catch (const std::exception &e) {
            failed(e.what());
        }
    }
    void stop_receive() {
        // Wake any bounded startup producer before joining the receiver.
        dispatch_closing=true;
        retire_dispatch();
        if (ingest)
            controller.request_reclaim();
        ingest.reset();
        dispatch_closing=false;
        receive_timing.reset();
        reset_input_metrics();
        timing->availability(TimingStage::Input, true, false);
        timing->availability(TimingStage::AccessUnit, true, false);
    }
    void cancel_background() {
        if (background_slot < 0)
            return;
        stop_decode();
        auto &s = controller.ai_slot(background_slot);
        std::lock_guard<std::mutex> l(s.mutex);
        s.channel.clear();
        s.generation = ++controller.next_generation_;
        // This existing release used to erase the last hidden-camera identity
        // before the final shutdown snapshot could see it.
        if(s.latest.sample && !s.latest.preview_consumed && s.latest.timing.telemetry)
            s.latest.timing.telemetry->diagnostic(s.latest.timing.frame,
                controller.stopping_.load()?"shutdown_latest_retired":"background_latest_released");
        s.latest = {};
        s.detections.clear();
        s.job_done = false;
        background_slot = -1;
        state.background_active = false;
    }
    bool continuous_search() const {
        return controller.options_.search.enabled && !controller.options_.search.reid_model.empty();
    }
    void continuous_background(Sample &sample, bool key) {
        if (background_slot < 0 && key) {
            for (unsigned i = 0; i < controller.background_slots_.size(); ++i) {
                auto &s = controller.background_slots_[i];
                std::lock_guard<std::mutex> l(s.mutex);
                if (!s.channel.empty())
                    continue;
                s.channel = state.source.id;
                s.generation = state.generation;
                s.latest = {};
                s.detections.clear();
                s.job_done = false;
                s.last_ai = {};
                background_slot = 16 + i;
                state.background_active = true;
                break;
            }
        }
        if (background_slot < 0) {
            state.background_state = "waiting for continuous AI capacity (16 hidden cameras)";
            return;
        }
        try {
            if (!transcoder && !decoder && key)
                start_decoder(gst_sample_get_caps(sample.get()), true, &sample);
            state.background_state = "continuous person tracking";
        } catch (const std::exception &e) {
            cancel_background();
            state.background_state = e.what();
        }
    }
    void poll_background() {
        if (background_slot < 0)
            return;
        auto &s = controller.ai_slot(background_slot);
        bool done = false, decoded = false;
        {
            std::lock_guard<std::mutex> l(s.mutex);
            done = s.job_done;
            decoded = bool(s.latest.sample);
            if (decoded && !background_counted) {
                state.background_decoded++;
                background_counted = true;
            }
            if (done) {
                state.background_ai++;
                state.background_detections = s.detections;
                state.background_capture_us = background_capture_us;
                background_result_at = s.detected_at;
            }
        }
        if (continuous_search()) {
            if (done) {
                std::lock_guard<std::mutex> l(s.mutex);
                s.job_done = false;
            }
            return;
        }
        if (done) {
            state.background_state = "waiting for keyframe";
            cancel_background();
        } else if (!decoded && Clock::now() >= background_deadline) {
            state.background_state = "decode timeout";
            cancel_background();
        } else if (decoded)
            state.background_state = "inference queued";
    }
    void background(Sample &sample, bool key) {
        if (!key || background_slot >= 0 || Clock::now() < background_due)
            return;
        for (unsigned i = 0; i < controller.background_slots_.size(); ++i) {
            auto &s = controller.background_slots_[i];
            {
                std::lock_guard<std::mutex> l(s.mutex);
                if (!s.channel.empty())
                    continue;
                s.channel = state.source.id;
                s.generation = state.generation;
                s.latest = {};
                s.detections.clear();
                s.error.clear();
                s.job_done = false;
                s.last_ai = {};
            }
            background_slot = 16 + i;
            state.background_active = true;
            background_counted = false;
            background_captured = Clock::now();
            background_capture_us = g_get_real_time();
            background_due =
                background_captured +
                std::chrono::seconds(controller.options_.background_ai_interval_seconds);
            background_deadline = background_captured + std::chrono::seconds(2);
            state.background_attempts++;
            state.background_state = "decoding keyframe";
            // A re-encoded camera already decodes every frame; the next one is used.
            if (transcoder)
                return;
            try {
                start_decoder(gst_sample_get_caps(sample.get()));
                auto *buffer = keyframes.input(gst_sample_get_buffer(sample.get()));
                if (!buffer)
                    throw std::runtime_error("Missing keyframe parameters");
                // Header prepending creates a new compressed buffer; retain the AU identity.
                set_frame_trace(buffer, frame_trace(gst_sample_get_buffer(sample.get())));
                stamp(0, GST_BUFFER_PTS(gst_sample_get_buffer(sample.get())));
                decode_timing->submit(buffer);
                const auto flow = gst_app_src_push_buffer(GST_APP_SRC(decode_input), buffer);
                if (flow != GST_FLOW_OK ||
                    gst_app_src_end_of_stream(GST_APP_SRC(decode_input)) != GST_FLOW_OK)
                    throw std::runtime_error("Keyframe submission failed");
            } catch (const std::exception &) {
                state.background_state = "keyframe decode failed";
                cancel_background();
            }
            return;
        }
        state.background_state = "waiting for capacity";
    }
    void stop_decode() {
        if(state.slot>=0)retire_presentation(controller.slots_[state.slot].presentation.close(),
            controller.stopping_.load()?"shutdown_fifo_retired":"preview_not_requested",8);
        // The transcoder owns the decode while it runs; see end_record().
        if (transcoder)
            return;
        input_dispatch.pause();
        if (decoder)
            controller.request_reclaim();
        if (controller.stopping_.load() && decoder && decode_input) {
            const auto before=Telemetry::monotonic_ns();
            gst_app_src_end_of_stream(GST_APP_SRC(decode_input));
            auto *bus=gst_element_get_bus(decoder);
            auto *message=gst_bus_timed_pop_filtered(bus,2*GST_SECOND,
                GstMessageType(GST_MESSAGE_EOS|GST_MESSAGE_ERROR));
            const int result=message && GST_MESSAGE_TYPE(message)==GST_MESSAGE_EOS ? 0 : message ? 2 : 1;
            if(result) state.error="Decoder shutdown drain failed";
            if(message) gst_message_unref(message);
            gst_object_unref(bus);
            diagnostic_event(state.source.id,state.generation,0,"decoder_drain",Telemetry::monotonic_ns(),
                Telemetry::monotonic_ns()-before,result);
        }
        if (decode_input) {
            gst_object_unref(decode_input);
            decode_input = nullptr;
        }
        destroy(decoder);
        decode_timing.reset();
        timing->activity(TimingStage::DecodeInput, false);
        timing->activity(TimingStage::DecodeOutput, false);
        timing->activity(TimingStage::Draw, false);
        timing->activity(TimingStage::Swap, false);
        decode_wait = true;
        decode_origin = GST_CLOCK_TIME_NONE;
        {
            std::lock_guard<std::mutex> lock(search_stamp_mutex);
            search_stamps.clear();
        }
    }
    void end_record() {
        recording_clock.clear();
        if(transcoder) input_dispatch.pause();
        if (recording) {
            recording->end();
            recording = nullptr;
        }
        // Frames still inside the transcoder are dropped; the next segment starts at
        // the next keyframe like any recording restart, and display falls back to its
        // own decoder meanwhile.
        if (transcoder) {
            transcoder.reset();
            stop_decode();
        }
        { std::lock_guard<std::mutex> lock(transcode_wall_mutex); transcode_wall.clear(); }
    }
    void failed(const std::string &error) {
        if (state.received && recording)
            state.gaps++;
        state.error = public_error(error, state.source);
        stop_receive();
        cancel_background();
        stop_decode();
        end_record();
        state.background_detections.clear();
        state.background_capture_us = 0;
        state.background_age_ms = 0;
        background_due = {};
        state.connection = "reconnecting";
        state.recording = "waiting";
        state.generation = ++controller.next_generation_;
        if (state.slot >= 0) {
            auto &s = controller.slots_[state.slot];
            std::lock_guard<std::mutex> l(s.mutex);
            s.generation = state.generation;
            s.latest = {};
            s.detections.clear();
        }
        const unsigned backoff = std::min(30u, 1u << std::min(retries++, 5u));
        const double jitter = 0.9 + (g_random_int() % 201) / 1000.0;
        reconnect = Clock::now() + std::chrono::milliseconds(unsigned(backoff * 1000 * jitter));
    }
    static GstPadProbeReturn allocation(GstPad *, GstPadProbeInfo *info, gpointer data) {
        auto *query = GST_PAD_PROBE_INFO_QUERY(info);
        if (GST_QUERY_TYPE(query) != GST_QUERY_ALLOCATION)
            return GST_PAD_PROBE_OK;
        gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, nullptr);
        gst_query_add_allocation_meta(query, GST_VIDEO_CROP_META_API_TYPE, nullptr);
        auto &channel=*static_cast<Channel*>(data);
        if(channel.state.slot>=0) {
            // GstVaBaseDec adds this downstream minimum to its DPB minimum.
            // Preserve its VA allocator and unbounded allocator maximum; only
            // reserve FIFO(3) + one producer surface above the existing minimum.
            if(gst_query_get_n_allocation_pools(query)==0)
                gst_query_add_allocation_pool(query,nullptr,0,4,0);
            else {
                GstBufferPool *pool=nullptr;guint size=0,minimum=0,maximum=0;
                gst_query_parse_nth_allocation_pool(query,0,&pool,&size,&minimum,&maximum);
                gst_query_set_nth_allocation_pool(query,0,pool,size,minimum+4,
                    maximum?maximum+4:0);
                if(pool)gst_object_unref(pool);
            }
        }
        return GST_PAD_PROBE_HANDLED;
    }
    void start_decoder(GstCaps *caps, bool live = false, const Sample *seed = nullptr) {
        const char *decode =
            codec_id() == 1
                ? "appsrc name=input is-live=true format=time block=false ! h264parse ! "
                  "vah264dec name=decode ! video/x-raw(memory:VAMemory),format=NV12 ! appsink "
                  "name=frames sync=false max-buffers=1 drop=false"
                : "appsrc name=input is-live=true format=time block=false ! h265parse ! "
                  "vah265dec name=decode ! video/x-raw(memory:VAMemory),format=NV12 ! appsink "
                  "name=frames sync=false max-buffers=1 drop=false";
        decoder = parse(decode);
        decode_timing = std::make_unique<PipelineTelemetry>(timing);
        decode_timing->background(background_slot >= 0);
        auto *decoder_element = gst_bin_get_by_name(GST_BIN(decoder), "decode");
        decode_timing->attach_decoder(decoder_element);
        gst_object_unref(decoder_element);
        auto *context = gst_context_new(GST_VA_DISPLAY_HANDLE_CONTEXT_TYPE_STR, TRUE);
        gst_context_set_va_display(context, controller.display_);
        gst_element_set_context(decoder, context);
        gst_context_unref(context);
        decode_input = gst_bin_get_by_name(GST_BIN(decoder), "input");
        gst_app_src_set_caps(GST_APP_SRC(decode_input), caps);
        g_object_set(decode_input, "max-bytes", guint64(compressed_limit), nullptr);
        auto *sink = gst_bin_get_by_name(GST_BIN(decoder), "frames");
        auto *pad = gst_element_get_static_pad(sink, "sink");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, allocation, this, nullptr);
        gst_object_unref(pad);
        GstAppSinkCallbacks cb{};
        cb.new_sample = decoded;
        gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cb, this, nullptr);
        gst_object_unref(sink);
        reset_presentation();
        if (gst_element_set_state(decoder, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
            throw std::runtime_error("Cannot start VA decoder");
        decode_wait = false;
        if(live) {
            if(!seed) throw std::logic_error("Live decoder needs a bootstrap AU");
            activate_dispatch(false,*seed);
        }
    }
    void start_record(GstCaps *caps, const Sample &seed) {
        const auto &t = state.source.transcode;
        if (!t.enabled) {
            open_recorder(caps, codec_id() == 1 ? RecordCodec::H264 : RecordCodec::H265);
            return;
        }
        // The transcoder's decode also feeds display and AI, shown or hidden, so the
        // stream is decoded once.
        stop_decode();
        decode_timing = std::make_unique<PipelineTelemetry>(timing);
        decode_timing->background(state.slot < 0);
        // Keyframes every two seconds, as most cameras send.
        const unsigned key = state.input_fps > 1 ? unsigned(std::lround(state.input_fps * 2)) : 60;
        Transcoder::Options options{t.codec,  t.rate_control,   t.bitrate_kbps, t.height,
                                    key,      compressed_limit, true};
        options.on_input = [this](GstBuffer *b) {
            if (decode_timing)
                decode_timing->submit(b);
        };
        transcoder = std::make_unique<Transcoder>(controller.display_, std::move(options), caps);
        decode_timing->attach_decoder(transcoder->decoder());
        transcoder->max_fps(controller.transcode_fps_limit_);
        auto *pad = gst_element_get_static_pad(transcoder->frames(), "sink");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM, allocation, this, nullptr);
        gst_object_unref(pad);
        GstAppSinkCallbacks cb{};
        cb.new_sample = decoded;
        gst_app_sink_set_callbacks(GST_APP_SINK(transcoder->frames()), &cb, this, nullptr);
        state.recording = "encoding";
        reset_presentation();
        activate_dispatch(true,seed);
    }
    // Moves re-encoded access units into the recorder.
    void drain_transcoder() {
        if (!transcoder)
            return;
        if (!transcoder->poll())
            throw std::runtime_error(transcoder->error());
        while (auto *raw = transcoder->pull()) {
            Sample sample(raw, gst_sample_unref);
            auto *buffer = gst_sample_get_buffer(raw);
            const auto pts = GST_BUFFER_PTS(buffer);
            std::optional<int64_t> wall;
            // Output is in presentation order; earlier entries were dropped frames.
            {
            std::lock_guard<std::mutex> lock(transcode_wall_mutex);
            for (auto it = transcode_wall.begin();
                 it != transcode_wall.end() && it->first <= pts;) {
                if (it->first == pts)
                    wall = it->second;
                it = transcode_wall.erase(it);
            }
            }
            output_format(gst_sample_get_caps(raw));
            output_bytes += gst_buffer_get_size(buffer);
            output_frames++;
            if (!recording) {
                if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT))
                    continue;
                open_recorder(gst_sample_get_caps(raw), state.source.transcode.codec);
            }
            if (!recording->push(raw, wall)) {
                end_record();
                state.gaps++;
                state.recording = "waiting for keyframe";
                return;
            }
            publish_recording_clock(raw);
        }
    }
    void output_format(GstCaps *caps) {
        if (!caps || gst_caps_is_empty(caps))
            return;
        const auto *format = gst_caps_get_structure(caps, 0);
        state.output_codec = codec_label(state.source.transcode.codec);
        int width = 0, height = 0;
        gst_structure_get_int(format, "width", &width);
        gst_structure_get_int(format, "height", &height);
        state.output_width = unsigned(std::max(0, width));
        state.output_height = unsigned(std::max(0, height));
    }
    void open_recorder(GstCaps *caps, RecordCodec codec) {
        SegmentRecorder::Options options;
        options.channel = state.source.id;
        options.camera_name = state.source.name;
        options.codec = codec;
        options.segment_seconds = controller.options_.segment_seconds;
        options.queue_limit_bytes = compressed_limit;
        options.on_destroyed = [this] { controller.request_reclaim(); };
        auto r = std::make_unique<SegmentRecorder>(*controller.catalog_, std::move(options), caps);
        recording = r.get();
        recordings.push_back(std::move(r));
        state.recording = "recording";
        if (state.error.rfind("Recording:", 0) == 0 || state.error.rfind("Storage:", 0) == 0)
            state.error.clear();
    }
    bool push(GstElement *input, const Sample &sample, GstClockTime &origin) {
        guint64 level{}, frames{}, duration{};
        g_object_get(input, "current-level-bytes", &level, nullptr);
        if (diagnostics_enabled()) {
            g_object_get(input, "current-level-buffers", &frames, "current-level-time", &duration, nullptr);
            const auto trace = frame_trace(gst_sample_get_buffer(sample.get()));
            timing->diagnostic(trace.frame, "appsrc_queue", level, frames, GST_BUFFER_PTS(gst_sample_get_buffer(sample.get())));
            timing->diagnostic(trace.frame, "appsrc_time", duration);
        }
        if (level > compressed_limit)
            return false;
        auto *original = gst_sample_get_buffer(sample.get());
        auto *b = gst_buffer_copy(original); // Header only; GstMemory refs stay shared.
        const auto pts = GST_BUFFER_PTS(b);
        if (!GST_CLOCK_TIME_IS_VALID(pts)) {
            gst_buffer_unref(b);
            return false;
        }
        if (origin == GST_CLOCK_TIME_NONE)
            origin = pts;
        if (pts < origin) {
            gst_buffer_unref(b);
            return false;
        }
        GST_BUFFER_PTS(b) = pts - origin;
        GST_BUFFER_DTS(b) =
            GST_CLOCK_TIME_IS_VALID(GST_BUFFER_DTS(b)) && GST_BUFFER_DTS(b) >= origin
                ? GST_BUFFER_DTS(b) - origin
                : GST_CLOCK_TIME_NONE;
        if (input == decode_input) {
            stamp(GST_BUFFER_PTS(b), pts);
            if (decode_timing)
                decode_timing->submit(b);
        }
        return gst_app_src_push_buffer(GST_APP_SRC(input), b) == GST_FLOW_OK;
    }
    void tick(bool space) {
        state.presentation_dropped=timing->counter_value(TimingCounter::PresentationDropped);
        if(state.presentation_dropped!=logged_display_drops && Clock::now()>=display_drop_log_at) {
            std::cerr<<"NVR live display drops camera="<<state.source.id
                     <<" delta="<<state.presentation_dropped-logged_display_drops
                     <<" total="<<state.presentation_dropped<<'\n';
            logged_display_drops=state.presentation_dropped;
            display_drop_log_at=Clock::now()+std::chrono::seconds(1);
        }
        if(state.slot>=0) {
            const auto failure=controller.slots_[state.slot].presentation.snapshot().error;
            if(!failure.empty())throw std::runtime_error("Presentation "+state.source.id+": "+failure);
        }
        DiagnosticControlSpan tick_span(state.source.id,"control_tick");
        if (receive_timing)
            receive_timing->background(state.slot < 0);
        // The shared transcoder decode outlives display changes.
        if (transcoder && decode_timing)
            decode_timing->background(state.slot < 0);
        timing->availability(TimingStage::PreviewReady, controller.options_.preview_enabled,
                             controller.options_.preview_enabled &&
                                 controller.presentation_observed_.load() && state.slot >= 0 &&
                                 controller.slots_[state.slot].preview_visible.load() &&
                                 state.connection == "live");
        recordings.erase(std::remove_if(recordings.begin(), recordings.end(),
                                        [this](auto &r) {
                                            const bool done = r->poll();
                                            if (r->consume_failure()) {
                                                record_failed = true;
                                                const auto error = r->error();
                                                if (!error.empty()) {
                                                    state.recording = "error";
                                                    state.error = "Recording: " +
                                                                  public_error(error, state.source);
                                                }
                                            }
                                            if (done && recording == r.get())
                                                recording = nullptr;
                                            return done;
                                        }),
                         recordings.end());
        if (record_failed.exchange(false)) {
            end_record();
            state.recording = "error";
            state.gaps++;
            record_retry = Clock::now() + std::chrono::seconds(5);
        }
        if (removed)
            return;
        poll_background();
        if (state.background_capture_us)
            state.background_age_ms =
                unsigned(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                                               background_result_at)
                             .count());
        if (!controller.options_.background_ai_enabled)
            state.background_state = "disabled";
        else if (state.slot >= 0)
            state.background_state = "display AI";
        else if (background_slot < 0 && state.background_state == "disabled")
            state.background_state = "waiting for keyframe";
        if (!space) {
            end_record();
            state.recording = "quota paused";
        }
        if (!ingest) {
            if (Clock::now() >= reconnect)
                start_receive();
            return;
        }
        const std::string error = ingest->poll();
        if (!error.empty()) {
            failed(error);
            return;
        }
        if (decoder) {
            auto *b = gst_element_get_bus(decoder);
            while (auto *m = gst_bus_pop_filtered(
                       b, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
                const bool error = GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR;
                gst_message_unref(m);
                if (background_slot >= 0) {
                    if (error) {
                        state.background_state = "keyframe decode failed";
                        cancel_background();
                    }
                } else {
                    state.error = "Decoder stopped; waiting for keyframe";
                    stop_decode();
                }
            }
            gst_object_unref(b);
        }
        if (ingest->take_overflow()) {
            cancel_background();
            keyframes.reset();
            stop_decode();
            end_record();
            state.gaps++;
        }
        auto packets = ingest->take();
        for (auto &sample : packets) {
            input_format(gst_sample_get_caps(sample.get()));
            state.received++;
            if (state.connection != "live")
                controller.request_reclaim();
            state.connection = "live";
            retries = 0;
            const bool key = !GST_BUFFER_FLAG_IS_SET(gst_sample_get_buffer(sample.get()),
                                                     GST_BUFFER_FLAG_DELTA_UNIT);
            try {
                if (space && Clock::now() >= record_retry) {
                    if (!recording && !transcoder && key)
                        start_record(gst_sample_get_caps(sample.get()),sample);
                    if (transcoder) {
                        // The receive producer already submitted this AU once.
                        // Recording output is drained below, independently of input dispatch.
                    } else if (recording) {
                        if(!recording->push(sample.get())) {
                            end_record();
                            state.gaps++;
                            state.recording = "waiting for keyframe";
                        } else publish_recording_clock(sample.get());
                    }
                }
            } catch (const std::exception &e) {
                state.recording = "error";
                state.error = public_error(e.what(), state.source);
                end_record();
                state.gaps++;
                record_retry = Clock::now() + std::chrono::seconds(5);
            }
            const bool independent =
                controller.options_.background_ai_enabled
                    ? keyframes.observe(gst_sample_get_buffer(sample.get()), codec_id())
                    : false;
            if (state.slot < 0 && controller.options_.background_ai_enabled) {
                if (continuous_search())
                    continuous_background(sample, key);
                else
                    background(sample, independent);
            }
            if (state.slot >= 0 && !transcoder) {
                try {
                    if (!decoder && key)
                        start_decoder(gst_sample_get_caps(sample.get()), true, &sample);
                    // Running AUs arrive directly from RtspIngest::on_sample.
                } catch (const std::exception &e) {
                    stop_decode();
                    state.error = e.what();
                }
            }
        }
        try {
            drain_transcoder();
        } catch (const std::exception &e) {
            state.recording = "error";
            state.error = "Recording: " + public_error(e.what(), state.source);
            end_record();
            state.gaps++;
            record_retry = Clock::now() + std::chrono::seconds(5);
        }
        if (state.slot >= 0) {
            auto &slot = controller.slots_[state.slot];
            DiagnosticControlSpan span(state.source.id,"control_status_slot");
            std::lock_guard<std::mutex> l(slot.mutex);
            state.decoded = slot.processed - decode_count_base;
            state.ai = slot.inferred - ai_count_base;
            state.ai_age_ms = slot.detected_at == Clock::time_point{}
                                  ? 0
                                  : unsigned(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 Clock::now() - slot.detected_at)
                                                 .count());
            if (!slot.error.empty())
                state.error = slot.error;
        }
    }
};
Controller::Controller(Options options) : options_(std::move(options)) {
    ai_runtime_->configure(options_.ai_fps, options_.search.interval_ms);
    if (options_.search.enabled)
        options_.background_ai_enabled = true;
    if (options_.batch != 1 && options_.batch != 2 && options_.batch != 4 && options_.batch != 8 &&
        options_.batch != 16)
        throw std::runtime_error("Invalid batch size");
    if (options_.background_ai_interval_seconds < 1 ||
        options_.background_ai_interval_seconds > 3600)
        throw std::runtime_error("Invalid background AI interval");
    if (!options_.segment_seconds || !options_.quota || options_.retention_minutes > 2147483647u)
        throw std::runtime_error("Invalid recording limits");
    display_ = gst_va_display_drm_new_from_path("/dev/dri/renderD128");
    if (!display_)
        throw std::runtime_error("Cannot open shared VA display");
    try {
        catalog_ = std::make_unique<Catalog>(options_.recording_root);
        search_index_ = std::make_unique<SearchIndex>(*catalog_);
        catalog_->analysis_hooks(
            {[runtime = ai_runtime_](size_t pending, unsigned dropped) {
                 runtime->queue("Analysis save queue", "", unsigned(pending), dropped);
             },
             [runtime = ai_runtime_](double ms, double wait, size_t frames) {
                 runtime->record("Analysis DB save", "", ms, wait, unsigned(frames));
             },
             [runtime = ai_runtime_](size_t frames) {
                 runtime->queue("Analysis save errors", "", 0, frames);
             }});
        if (options_.results.method != "none")
            publisher_ = std::make_unique<ResultPublisher>(options_.results);
        for (auto &s : slots_) {
            s.va = std::make_unique<VaSession>(
                static_cast<VADisplay>(gst_va_display_get_va_dpy(display_)));
            s.ai_surface = s.va->create_bgra(640, 640);
            s.image = std::make_unique<ImportedSurface>(s.va->export_surface(s.ai_surface));
            if (options_.preview_enabled)
                for (auto &p : s.preview)
                    p.surface = s.va->create_bgra(max_video_width, max_video_height);
        }
        if (options_.background_ai_enabled)
            for (auto &s : background_slots_) {
                s.va = std::make_unique<VaSession>(
                    static_cast<VADisplay>(gst_va_display_get_va_dpy(display_)));
                s.ai_surface = s.va->create_bgra(640, 640);
                s.image = std::make_unique<ImportedSurface>(s.va->export_surface(s.ai_surface));
            }
        // Keep the startup probe separate from the live processing contexts.
        VaSession gate(static_cast<VADisplay>(gst_va_display_get_va_dpy(display_)));
        const auto test = gate.create_nv12(640, 360, true), output = gate.create_bgra(640, 360);
        try {
            gate.scale(test, output, 640, 360, 640, 360);
            // scale() verifies the accepted VPE submission and exact target fence.
        } catch (...) {
            gate.destroy_surface(test);
            gate.destroy_surface(output);
            throw;
        }
        gate.destroy_surface(test);
        gate.destroy_surface(output);
    } catch (...) {
        for (auto &s : slots_) {
            s.image.reset();
            if (s.va) {
                for (auto &p : s.preview) {
                    s.va->destroy_surface(p.surface);
                    p.surface = VA_INVALID_SURFACE;
                }
                s.va->destroy_surface(s.ai_surface);
                s.ai_surface = VA_INVALID_SURFACE;
                s.va.reset();
            }
        }
        for (auto &s : background_slots_) {
            s.image.reset();
            if (s.va)
                s.va->destroy_surface(s.ai_surface);
            s.ai_surface = VA_INVALID_SURFACE;
            s.va.reset();
        }
        gst_object_unref(display_);
        display_ = nullptr;
        throw;
    }
    if (options_.search.enabled) {
        search_ =
            std::make_unique<SearchService>(*search_index_, display_, options_.search, ai_runtime_);
        // HIP graph capture must finish before the live GPU schedulers start.
        search_->wait_initialized();
    }
    control_ = std::thread([this] { loop(); });
    gpu_ = std::thread([this] { gpu_loop(); });
    preview_ = std::thread([this] { preview_loop(); });
    for (const auto &s : options_.sources)
        add(s);
    initial_enqueued_ = true;
}
Controller::~Controller() {
    shutdown();
    for (auto &s : slots_) {
        s.latest = {};
        s.preview_va.reset();
        s.image.reset();
        for (auto &p : s.preview) {
            s.va->destroy_surface(p.surface);
            p.surface = VA_INVALID_SURFACE;
        }
        s.va->destroy_surface(s.ai_surface);
        s.ai_surface = VA_INVALID_SURFACE;
        s.va.reset();
    }
    for (auto &s : background_slots_) {
        s.latest = {};
        s.image.reset();
        if (s.va)
            s.va->destroy_surface(s.ai_surface);
        s.ai_surface = VA_INVALID_SURFACE;
        s.va.reset();
    }
    if (display_)
        gst_object_unref(display_);
}
void Controller::request_shutdown() {
    const auto requested=Telemetry::monotonic_ns();
    if(stopping_.exchange(true))return;
    diagnostic_event("controller",0,0,"shutdown_requested",requested);
    for(auto &slot:slots_)
        retire_presentation(slot.presentation.close(),"shutdown_fifo_retired");
}
void Controller::shutdown() {
    if(shutdown_started_.exchange(true))return;
    request_shutdown();
    if (control_.joinable())
        control_.join();
    if (gpu_.joinable())
        gpu_.join();
    if (preview_.joinable())
        preview_.join();
    if (search_)
        search_->shutdown();
    channels_.clear();
    // Log the existing shutdown dispositions after producers have joined.
    // No frame is newly discarded here; the destructor already releases these.
    auto record_shutdown=[](const auto &slot) {
        if(slot.latest.sample && !slot.latest.preview_consumed && slot.latest.timing.telemetry)
            slot.latest.timing.telemetry->diagnostic(slot.latest.timing.frame,"shutdown_latest_retired");
        for(const auto &plane:slot.preview)
            if(plane.state==1 && plane.timing.telemetry)
                plane.timing.telemetry->diagnostic(plane.timing.frame,"shutdown_preview_retired");
    };
    for(const auto &slot:slots_) record_shutdown(slot);
    for(const auto &slot:background_slots_) record_shutdown(slot);
    if(const auto started=copy_audit_started_ns_.load();started && amdva_audit_errors) {
        const auto ended=Telemetry::monotonic_ns();
        const auto violations=amdva_audit_errors(),copies=amdva_audit_copies(),bytes=amdva_audit_bytes();
        diagnostic_event("copy-audit",0,0,"copy_audit_result",ended,violations,copies);
        std::cerr << "NVR copy audit complete violations=" << violations << " copies=" << copies
                  << " bytes=" << bytes << '\n';
        if(const auto *directory=std::getenv("AMDVA_DIAGNOSTICS_DIR");directory && *directory) {
            std::ofstream out(std::filesystem::path(directory)/"copy-audit-summary.json");
            out << "{\"requested\":true,\"active_start_ns\":" << started << ",\"active_end_ns\":" << ended
                << ",\"violations\":" << violations << ",\"copies\":" << copies << ",\"bytes\":" << bytes << "}\n";
            if(!out) std::cerr << "Copy audit summary write failed\n";
        }
        if(amdva_audit_stop) amdva_audit_stop();
    }
    AmdvaVpeAuditStats vpe{};
    if(amdva_vpe_audit_snapshot && amdva_vpe_audit_snapshot(&vpe,sizeof(vpe))) {
        std::cerr<<"NVR VPE audit complete scopes="<<vpe.scopes<<" completed="<<vpe.completed
                 <<" failed="<<vpe.failed<<" active="<<vpe.active_scope<<'\n';
        if(const auto *directory=std::getenv("AMDVA_DIAGNOSTICS_DIR");directory && *directory) {
            std::ofstream out(std::filesystem::path(directory)/"vpe-audit-summary.json");
            out<<"{\"schema\":1,\"method\":\"binary-syncobj-fence\",\"scopes\":"<<vpe.scopes
               <<",\"completed\":"<<vpe.completed<<",\"failed\":"<<vpe.failed
               <<",\"aborted\":"<<vpe.aborted<<",\"submissions\":"<<vpe.submissions
               <<",\"rejected_submissions\":"<<vpe.rejected_submissions
               <<",\"foreign_operations\":"<<vpe.foreign_operations<<",\"active_scope\":"<<vpe.active_scope
               <<",\"observer_wait_ns\":"<<vpe.wait_ns<<",\"maximum_observer_wait_ns\":"<<vpe.maximum_wait_ns<<"}\n";
            if(!out)std::cerr<<"VPE audit summary write failed\n";
        }
    }
    diagnostic_event("controller",0,0,"shutdown_drained",Telemetry::monotonic_ns());
}
void Controller::retention_minutes(unsigned minutes) {
    if (minutes > 2147483647u)
        throw std::invalid_argument("Invalid retention_minutes");
    command([this, minutes] {
        options_.retention_minutes = minutes;
        expired_delete_ = {};
    });
}
void Controller::delete_closed_recordings(std::shared_future<void> released) {
    command([this, released] {
        if (deleting_)
            return;
        const auto operation = deletion_status_.operation + 1;
        try {
            manual_delete_ = catalog_->begin_delete();
            deleting_ = true;
            deletion_release_ = released;
            std::lock_guard<std::mutex> lock(mutex_);
            deletion_status_ = {};
            deletion_status_.operation = operation;
            deletion_status_.state = released.valid() ? "waiting" : "running";
            deletion_status_.total = manual_delete_.total;
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            deletion_status_ = {};
            deletion_status_.operation = operation;
            deletion_status_.state = "error";
            deletion_status_.failed = 1;
        }
    });
}
void Controller::cancel_recording_deletion() {
    command([this] {
        deleting_ = false;
        manual_delete_.pending.clear();
        deletion_release_ = {};
        std::lock_guard<std::mutex> lock(mutex_);
        deletion_status_.state = "cancelled";
    });
}
DeletionStatus Controller::deletion_status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return deletion_status_;
}
void Controller::request_reclaim() {
    // Transient GStreamer worker groups leave free pages in glibc arenas. Allow
    // queued frame owners to drain, then return those pages without changing
    // the resource guard or retaining every reconnect's high-water mark.
    reclaim_pending_ = true;
    reclaim_at_ = std::max(reclaim_at_, Clock::now() + std::chrono::seconds(1));
}
void Controller::command(std::function<void()> f) {
    std::lock_guard<std::mutex> l(mutex_);
    if (!stopping_)
        commands_.push_back(std::move(f));
}
void Controller::add(Source source) {
    command([this, source]() mutable {
        if (source.id.empty())
            source.id = identifier();
        if (source.id.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
            std::string::npos)
            return;
        if (source.uri.rfind("rtsp://", 0) != 0 && source.uri.rfind("rtsps://", 0) != 0)
            return;
        for (const auto &c : channels_)
            if (c->state.source.id == source.id && !c->removed)
                return;
        const bool chosen = source.selected;
        auto c = std::make_unique<Channel>(*this, std::move(source));
        if (chosen)
            assign(*c, true);
        channels_.push_back(std::move(c));
    });
}
void Controller::remove(const std::string &id) {
    command([this, id] {
        for (auto &c : channels_)
            if (c->state.source.id == id && !c->removed) {
                assign(*c, false);
                c->cancel_background();
                c->removed = true;
                c->stop_receive();
                c->end_record();
            }
    });
}
void Controller::select(const std::string &id, bool value) {
    command([this, id, value] {
        for (auto &c : channels_)
            if (c->state.source.id == id && !c->removed)
                assign(*c, value);
    });
}
void Controller::assign(Channel &c, bool value) {
    if (value == c.state.source.selected)
        return;
    if (value) {
        for (unsigned i = 0; i < slots_.size(); ++i) {
            auto &s = slots_[i];
            std::lock_guard<std::mutex> l(s.mutex);
            if (s.channel.empty()) {
                c.cancel_background();
                c.state.background_detections.clear();
                c.state.background_capture_us = 0;
                c.state.slot = int(i);
                c.state.source.selected = true;
                if (c.state.error == "Maximum 16 displayed channels")
                    c.state.error.clear();
                c.state.generation = ++next_generation_;
                s.channel = c.state.source.id;
                s.preview_visible=preview_selection_.contains(s.channel);
                s.generation = c.state.generation;
                s.detections.clear();
                s.latest = {};
                s.error.clear();
                s.last_ai = {};
                c.decode_count_base = s.processed;
                c.ai_count_base = s.inferred;
                c.update_dispatch_mode();
                return;
            }
        }
        c.state.error = "Maximum 16 displayed channels";
    } else {
        c.stop_decode();
        const int i = c.state.slot;
        c.state.slot = -1;
        c.state.source.selected = false;
        c.background_due = {};
        c.state.generation = ++next_generation_;
        if (i >= 0) {
            auto &s = slots_[i];
            std::lock_guard<std::mutex> l(s.mutex);
            s.channel.clear();
            s.generation = c.state.generation;
            s.latest = {};
            s.detections.clear();
        }
        c.update_dispatch_mode();
    }
}
std::vector<Status> Controller::status() const {
    std::lock_guard<std::mutex> l(mutex_);
    return statuses_;
}
std::string Controller::fatal() const {
    std::lock_guard<std::mutex> l(mutex_);
    return fatal_;
}
void Controller::loop() {
    try {
        bool space = true;
        std::string storage_error;
        auto quota_at = Clock::now();
        auto stats_at = Clock::now();
        size_t background_cursor = 0;
        std::map<std::string, std::pair<uint64_t, uint64_t>> counts;
        while (true) {
            DiagnosticControlSpan loop_span("controller","control_loop");
            std::deque<std::function<void()>> work;
            {
                std::lock_guard<std::mutex> l(mutex_);
                work.swap(commands_);
            }
            for (auto &f : work)
                f();
            if (stopping_)
                break;
            if (Clock::now() >= quota_at) {
                DiagnosticControlSpan span("controller","control_quota");
                try {
                    space = catalog_->enforce(options_.quota, options_.reserve);
                    if (options_.retention_minutes && expired_delete_.done())
                        expired_delete_ = catalog_->begin_delete(
                            g_get_real_time() / 1000 - int64_t(options_.retention_minutes) * 60000);
                    storage_error.clear();
                } catch (const std::exception &e) {
                    space = false;
                    storage_error = e.what();
                }
                quota_at = Clock::now() + std::chrono::seconds(1);
            }
            if (deleting_ && (!deletion_release_.valid() ||
                              deletion_release_.wait_for(std::chrono::seconds(0)) ==
                                  std::future_status::ready)) {
                bool released = true;
                if (deletion_release_.valid()) {
                    try {
                        deletion_release_.get();
                    } catch (...) {
                        manual_delete_.failed++;
                        manual_delete_.pending.clear();
                        released = false;
                    }
                    deletion_release_ = {};
                }
                if (released) {
                    DiagnosticControlSpan span("controller","control_manual_delete");
                    catalog_->delete_batch(manual_delete_);
                }
                deleting_ = !manual_delete_.done();
                std::lock_guard<std::mutex> lock(mutex_);
                deletion_status_ = {deleting_
                                        ? "running"
                                        : (manual_delete_.failed || manual_delete_.protected_files
                                               ? "partial"
                                               : "complete"),
                                    manual_delete_.total,
                                    manual_delete_.processed,
                                    manual_delete_.deleted,
                                    manual_delete_.bytes,
                                    manual_delete_.protected_files,
                                    manual_delete_.failed,
                                    deletion_status_.operation};
            } else if (!deleting_ && !expired_delete_.done()) {
                DiagnosticControlSpan span("controller","control_expiry_delete");
                catalog_->delete_batch(expired_delete_);
            }
            // Older background deadlines win; rotate ties across equal-deadline cameras.
            std::vector<Channel *> order;
            for (size_t i = 0, n = channels_.size(); i < n; ++i)
                order.push_back(channels_[(background_cursor + i) % n].get());
            std::stable_sort(order.begin(), order.end(), [](const auto *a, const auto *b) {
                return a->background_due < b->background_due;
            });
            for (auto *c : order) {
                c->tick(space);
                if (!storage_error.empty() && !c->removed) {
                    c->state.recording = "storage paused";
                    c->state.error = "Storage: " + storage_error;
                }
            }
            background_cursor++;
            {
                std::lock_guard<std::mutex> lock(telemetry_mutex_);
                for (const auto &c : channels_)
                    if (c->removed && c->recordings.empty()) {
                        auto it = telemetry_.find(c->state.source.id);
                        if (it != telemetry_.end() && it->second == c->timing)
                            telemetry_.erase(it);
                    }
            }
            channels_.erase(
                std::remove_if(channels_.begin(), channels_.end(),
                               [](const auto &c) { return c->removed && c->recordings.empty(); }),
                channels_.end());
            if (reclaim_pending_ && Clock::now() >= reclaim_at_) {
                DiagnosticControlSpan span("controller","control_malloc_trim");
                malloc_trim(0);
                reclaim_pending_ = false;
                reclaim_at_ = Clock::now() + std::chrono::seconds(5);
            }
            if (Clock::now() >= transcode_rate_at_) {
                pace_transcoding();
                transcode_rate_at_ = Clock::now() + std::chrono::seconds(2);
            }
            const double elapsed = std::chrono::duration<double>(Clock::now() - stats_at).count();
            std::vector<Status> snapshot;
            for (auto &c : channels_)
                if (!c->removed) {
                    c->update_input_rates();
                    if (elapsed >= 1) {
                        auto &old = counts[c->state.source.id];
                        c->state.display_fps =
                            (c->state.decoded >= old.first ? c->state.decoded - old.first : 0) /
                            elapsed;
                        c->state.ai_fps =
                            (c->state.ai >= old.second ? c->state.ai - old.second : 0) / elapsed;
                        old = {c->state.decoded, c->state.ai};
                    }
                    snapshot.push_back(c->state);
                }
            if (elapsed >= 1) {
                stats_at = Clock::now();
                for (auto it = counts.begin(); it != counts.end();) {
                    if (std::none_of(channels_.begin(), channels_.end(), [&](const auto &c) {
                            return !c->removed && c->state.source.id == it->first;
                        }))
                        it = counts.erase(it);
                    else
                        ++it;
                }
            }
            {
                std::lock_guard<std::mutex> l(mutex_);
                statuses_ = std::move(snapshot);
                if (initial_enqueued_ && commands_.empty())
                    status_ready_ = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // Do not call a previously admitted RTP group a shutdown cancellation.
        // Let those groups assemble first; later packets have explicit post-stop
        // identities. A timeout remains a validation error, never a guessed match.
        const auto receive_cutoff=Telemetry::monotonic_ns();
        const auto receive_deadline=Clock::now()+std::chrono::milliseconds(500);
        bool unassembled=false;
        do {
            unassembled=false;
            for(const auto &c:channels_)
                if(c->receive_timing && c->receive_timing->unassembled_before(receive_cutoff)) unassembled=true;
            if(!unassembled || Clock::now()>=receive_deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while(true);
        if(unassembled) diagnostic_event("controller",0,0,"receive_drain_failed",Telemetry::monotonic_ns(),1);
        for (auto &c : channels_) c->stop_receive();
        for (auto &c : channels_) {
            c->cancel_background();
            c->stop_decode();
            c->end_record();
        }
        const auto until = Clock::now() + std::chrono::seconds(5);
        bool pending = true;
        while (pending && Clock::now() < until) {
            pending = false;
            for (auto &c : channels_) {
                c->recordings.erase(std::remove_if(c->recordings.begin(), c->recordings.end(),
                                                   [](auto &r) { return r->poll(); }),
                                    c->recordings.end());
                pending |= !c->recordings.empty();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        {
            std::vector<Status> final;
            for (const auto &c : channels_)
                if (!c->removed) {
                    auto state = c->state;
                    state.connection = "stopped";
                    state.recording = "closed";
                    final.push_back(std::move(state));
                }
            std::lock_guard<std::mutex> lock(mutex_);
            statuses_ = std::move(final);
            if (initial_enqueued_)
                status_ready_ = true;
        }
        channels_.clear();
    } catch (const std::exception &e) {
        std::lock_guard<std::mutex> l(mutex_);
        fatal_ = e.what();
        std::cerr<<"NVR control loop failed: "<<fatal_<<'\n';
        diagnostic_event("controller",0,0,"control_loop_failed",Telemetry::monotonic_ns(),1);
    }
}
void Controller::preview_loop() {
    try {
        std::array<Sample, 16> previous;
        // Each producer alone owns its scaling context and mutable geometry.
        std::array<PreviewSlot, 16> work;
        std::array<uint64_t, 16> blocked{};
        const auto model_info = ModelInfo::load(options_.model_info);
        while (!stopping_) {
            for (unsigned index = 0; index < slots_.size(); ++index) {
                auto &s = slots_[index];
                const auto preview_wait_start=Telemetry::monotonic_ns();
                std::unique_lock<std::mutex> l(s.mutex);
                const auto preview_acquired=Telemetry::monotonic_ns();
                if (s.channel.empty() || !s.latest.sample) {
                    previous[index].reset();
                    continue;
                }
                auto it = std::find_if(s.preview.begin(), s.preview.end(),
                                       [](const auto &p) { return p.state == 0; });
                const bool preview = options_.preview_enabled && presentation_observed_.load() &&
                                     s.preview_visible.load();
                auto entry=preview?s.presentation.peek():
                    std::optional<PresentationFifo<Frame>::Entry>{};
                if(preview && (!entry || Telemetry::monotonic_ns()<entry->due_ns-entry->period_ns))
                    continue;
                if(!preview && s.latest.sample==previous[index])continue;
                if (preview && it == s.preview.end()) {
                    if (s.latest.timing.telemetry && blocked[index] != s.latest.timing.frame.sequence) {
                        blocked[index]=s.latest.timing.frame.sequence;
                        s.latest.timing.telemetry->diagnostic(s.latest.timing.frame, "preview_full");
                    }
                    continue;
                }
                if(preview) {
                    entry=s.presentation.pop(entry->generation,entry->serial);
                    if(!entry)continue;
                    if(entry->frame.timing.telemetry)
                        entry->frame.timing.telemetry->diagnostic(entry->frame.timing.frame,"fifo_exit");
                }
                auto frame = preview?entry->frame:s.latest;
                auto *buffer = gst_sample_get_buffer(frame.sample.get());
                if (gst_va_buffer_peek_display(buffer) != display_)
                    throw std::runtime_error("Decoder did not use the shared VA display");
                if (options_.tracking_enabled) {
                    if (!s.tracker || s.tracking_generation != frame.generation) {
                        s.tracker = std::make_unique<MetadataTracker>(model_info);
                        s.tracking_generation = frame.generation;
                        s.tracking_observation = 0;
                        s.tracked.clear();
                    }
                    const bool observed = s.inferred != s.tracking_observation &&
                                          s.detection_width == frame.width &&
                                          s.detection_height == frame.height;
                    s.tracked = s.tracker->process(
                        frame.sample.get(), observed ? &s.detections : nullptr,
                        {s.channel, frame.generation, s.sequence, GST_BUFFER_PTS(buffer)});
                    if (observed) {
                        s.tracking_observation = s.inferred;
                        if (publisher_) {
                            AnalysisResult result;
                            for (const auto &tracked : s.tracked) {
                                Instance object;
                                object.box = tracked.box;
                                object.tracking_id = tracked.id;
                                if (object.box.class_id >= 0)
                                    result.objects.push_back(object);
                            }
                            auto *metadata = gst_buffer_new();
                            GST_BUFFER_PTS(metadata) = GST_BUFFER_PTS(buffer);
                            attach_analysis(
                                metadata, std::move(result), model_info,
                                ImageTransform::make(frame.width, frame.height, frame.width,
                                                     frame.height, false),
                                {s.channel, frame.generation, s.sequence, GST_BUFFER_PTS(buffer)});
                            publisher_->publish(analysis_json(metadata));
                            gst_buffer_unref(metadata);
                        }
                    }
                }
                if (!preview) {
                    if(frame.timing.telemetry) frame.timing.telemetry->diagnostic(frame.timing.frame,"preview_not_requested",
                        (!options_.preview_enabled?1:0)|(!presentation_observed_.load()?2:0)|(!s.preview_visible.load()?4:0));
                    s.latest.preview_consumed = true;
                    previous[index] = frame.sample;
                    s.processed++;
                    continue;
                }
                auto &job=work[index];
                job.target_width=s.target_width;job.target_height=s.target_height;
                const auto generation=s.generation,sequence=s.sequence+1;
                // Retain the decoded GstSample before releasing the slot lock.
                // Mark only this input consumed; a newer decoded frame can arrive.
                s.latest.preview_consumed=true;previous[index]=frame.sample;
                const auto reserved=Telemetry::monotonic_ns();
                if(frame.timing.telemetry) frame.timing.telemetry->diagnostic(frame.timing.frame,"preview_lock",preview_acquired-preview_wait_start,reserved-preview_acquired);
                const bool published=detail::publish_preview_unlocked(*it,l,[&](Plane &prepared) {
                    const auto scale_start=Telemetry::monotonic_ns();
                    prepare_preview(display_,job,prepared,frame.sample.get(),frame.generation,sequence);
                    if(frame.timing.telemetry) frame.timing.telemetry->diagnostic(frame.timing.frame,"preview_scale",Telemetry::monotonic_ns()-scale_start);
                    frame.timing.mark(TimingStage::PreviewReady);prepared.timing=frame.timing;
                    prepared.original_width=frame.width;prepared.original_height=frame.height;
                    prepared.time=frame.received;
                    prepared.presentation_due_ns=entry->due_ns;
                    prepared.presentation_period_ns=entry->period_ns;
                    prepared.presentation_confirmed=false;
                },[&]{return s.generation==generation && !stopping_.load() &&
                    presentation_observed_.load() && s.preview_visible.load();});
                if(published) {
                    if(s.latest.sample==frame.sample)s.latest.timing=frame.timing;
                    s.sequence=sequence;++s.processed;
                }else if(frame.timing.telemetry) {
                    frame.timing.telemetry->diagnostic(frame.timing.frame,
                        stopping_.load()?"shutdown_preview_retired":
                        s.generation==generation?"presentation_suspended_retired":"presentation_generation_retired");
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    } catch (const std::exception &e) {
        std::lock_guard<std::mutex> l(mutex_);
        fatal_ = std::string("Preview: ") + e.what();
    }
}
void Controller::gpu_loop() {
    try {
        hip_check(hipSetDevice(0), "AI device");
        BatchOptions configuration;
        configuration.measured = [runtime = ai_runtime_, ready = &inference_ready_](
                                     double inference, double post, unsigned batch) {
            if (!ready->load())
                return;
            runtime->record("YOLO inference", "", inference, 0, batch);
            runtime->record("YOLO postprocess", "", post, 0, batch);
        };
        configuration.model = options_.model;
        configuration.model_info = options_.model_info;
        configuration.size = options_.batch;
        configuration.timeout_ms = 0;
        const auto prefix = "nvr/" + std::to_string(reinterpret_cast<uintptr_t>(this));
        if (options_.search.enabled && !options_.search.reid_model.empty())
            configuration.confidence = .1f;
        configuration.instance_id = prefix + "/batch";
        auto batch = SharedBatchExecutor::acquire(configuration);
        const auto &info = batch->info();
        const PixelNormalization defaults;
        if (info.task != "detection" || !info.width || !info.height || info.width > 640 ||
            info.height > 640 || info.classes != 80 || info.input_name != "images" ||
            !info.letterbox || info.normalization.bgr != defaults.bgr ||
            info.normalization.scale != defaults.scale ||
            info.normalization.padding != defaults.padding ||
            info.normalization.mean != defaults.mean ||
            info.normalization.stddev != defaults.stddev)
            throw std::runtime_error(
                "NVR streaming analytics requires YOLOv8-compatible COCO80 up to 640x640 "
                "with default RGB letterbox normalization");
        std::shared_ptr<SharedBatchExecutor> single;
        if (options_.batch > 1) {
            configuration.model = options_.single_model;
            configuration.size = 1;
            configuration.instance_id = prefix + "/single";
            single = SharedBatchExecutor::acquire(configuration);
        }
        auto stream=std::make_shared<HipStream>(0);
        auto input = std::make_shared<DeviceBuffer>(size_t(options_.batch) * 3 * info.width *
                                                    info.height * sizeof(float),stream);
        auto view = [&](unsigned slot) {
            auto *ptr =
                static_cast<float *>(input->data()) + size_t(slot) * 3 * info.width * info.height;
            return std::shared_ptr<DeviceBuffer>(
                new DeviceBuffer(ptr, size_t(3) * info.width * info.height * sizeof(float)),
                [owner = input](DeviceBuffer *p) { delete p; });
        };
        ImagePacker pack;
        // Prime both model shapes before enabling the live copy audit and sampling
        // resource baselines; lazy kernels/workspaces must not first appear on
        // a later reconnect's single-image batch.
        {
            pack.pad(input->data(), 0, options_.batch, info.width, info.height,{},stream->get());
            stream->finish();
            std::vector<InferenceRequest> requests;
            for (unsigned i = 0; i < options_.batch; i++)
                requests.push_back(
                    {{"images", TensorType::Float32, {1, 3, info.height, info.width}, view(i), 0},
                     prefix + "/warmup/" + std::to_string(i),
                     0,
                     0});
            auto warmup = batch->submit_many(std::move(requests));
            for (auto &result : warmup)
                (void)result.get();
            if (single)
                (void)single
                    ->submit({{"images",
                               TensorType::Float32,
                               {1, 3, info.height, info.width},
                               view(0),
                               0},
                              prefix + "/warmup/single",
                              0,
                              0})
                    .get();
        }
        // The GUI launcher already preloads the auditor when requested. Start
        // it after model warmup, just as the hardware validation tools do.
        const auto *audit_requested=std::getenv("AMDVA_COPY_AUDIT");
        if(audit_requested && std::string(audit_requested)=="1") {
            if(!amdva_audit_start || !amdva_audit_errors || !amdva_audit_copies || !amdva_audit_bytes)
                throw std::runtime_error("Copy audit requested but auditor hooks are missing");
            amdva_audit_start();
            copy_audit_started_ns_=Telemetry::monotonic_ns();
            std::cerr << "NVR copy audit active ns=" << copy_audit_started_ns_.load() << '\n';
        }
        inference_ready_ = true;
        std::array<Sample, 32> previous;
        unsigned background_cursor = 0;
        unsigned cursor = 0;
        double rate = ai_runtime_->fps();
        auto waiting = Clock::time_point{};
        auto phase_origin = Clock::now();
        double phase_rate = rate;
        std::map<std::string, Clock::time_point> due;
        std::map<std::string, unsigned> ranks;
        for (unsigned i = 0; i < options_.sources.size(); i++)
            ranks[options_.sources[i].id] = i;
        const auto groups =
            std::max<size_t>(1, (options_.sources.size() + options_.batch - 1) / options_.batch);
        struct Item {
            unsigned slot;
            Frame frame;
            std::string channel;
            unsigned sw, sh, px, py;
        };
        while (!stopping_) {
            rate = ai_runtime_->fps();
            if (rate != phase_rate) {
                phase_rate = rate;
                phase_origin = Clock::now();
                due.clear();
            }
            const auto cadence = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(1.0 / rate));
            std::vector<Item> items;
            const auto now = Clock::now();
            std::vector<unsigned> order;
            for (unsigned i = 0; i < background_slots_.size(); ++i) {
                const unsigned index = 16 + (background_cursor + i) % background_slots_.size();
                auto &s = ai_slot(index);
                std::lock_guard<std::mutex> l(s.mutex);
                if (!s.channel.empty() && s.latest.sample &&
                    (!s.job_done ||
                     (options_.search.enabled && !options_.search.reid_model.empty()))) {
                    order.push_back(index);
                    break;
                }
            }
            for (unsigned i = 0; i < 16; ++i)
                order.push_back((cursor + i) % 16);
            for (unsigned i = 0; i < background_slots_.size(); ++i) {
                const unsigned index = 16 + (background_cursor + i) % background_slots_.size();
                if (order.empty() || order.front() != index)
                    order.push_back(index);
            }
            for (const auto index : order) {
                if (items.size() >= options_.batch)
                    break;
                if (index >= 16 &&
                    std::any_of(items.begin(), items.end(),
                                [](const auto &v) { return v.slot < 16; }) &&
                    std::any_of(items.begin(), items.end(),
                                [](const auto &v) { return v.slot >= 16; }))
                    continue;
                auto &s = ai_slot(index);
                std::lock_guard<std::mutex> l(s.mutex);
                if (s.channel.empty() || !s.latest.sample) {
                    previous[index].reset();
                    continue;
                }
                if ((s.job_done &&
                     !(options_.search.enabled && !options_.search.reid_model.empty())) ||
                    s.latest.sample == previous[index] ||
                    ((index < 16 ||
                      (options_.search.enabled && !options_.search.reid_model.empty())) &&
                     now <
                         due.try_emplace(s.channel,
                                         phase_origin +
                                             cadence * (ranks[s.channel] / options_.batch) / groups)
                             .first->second))
                    continue;
                const auto &f = s.latest;
                const double scale =
                    std::min(double(info.width) / f.width, double(info.height) / f.height);
                const unsigned sw = std::max(1u, unsigned(std::lround(f.width * scale))),
                               sh = std::max(1u, unsigned(std::lround(f.height * scale)));
                items.push_back(
                    {index, f, s.channel, sw, sh, (info.width - sw) / 2, (info.height - sh) / 2});
            }
            if (items.empty()) {
                waiting = {};
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (waiting == Clock::time_point{})
                waiting = now;
            if (items.size() < options_.batch && now - waiting < std::chrono::milliseconds(20)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            waiting = {};
            // Decoders hand frames on before completion; wait before reading their pixels.
            for (auto &item : items) {
                const auto synced = Clock::now();
                auto *buffer = gst_sample_get_buffer(item.frame.sample.get());
                va_check(vaSyncSurface(
                             static_cast<VADisplay>(gst_va_display_get_va_dpy(display_)),
                             gst_va_buffer_get_surface(buffer)),
                         "Detection frame decode");
                ai_runtime_->record("Decoded frame wait", item.channel,
                                    std::chrono::duration<double, std::milli>(Clock::now() - synced)
                                        .count());
            }
            const auto waiting_gpu = Clock::now();

            const auto started = Clock::now();
            unsigned valid = 0;
            std::vector<Item> prepared;
            for (auto &item : items) {
                auto &s = ai_slot(item.slot);
                std::lock_guard<std::mutex> l(s.mutex);
                if (s.channel != item.channel || s.generation != item.frame.generation)
                    continue;
                auto *buffer = gst_sample_get_buffer(item.frame.sample.get());
                if (gst_va_buffer_peek_display(buffer) != display_)
                    throw std::runtime_error("AI VA display mismatch");
                const std::array<unsigned, 4> geometry{item.frame.width, item.frame.height, item.sw,
                                                       item.sh};
                if (s.ai_geometry[0] && s.ai_geometry != geometry) {
                    s.va.reset();
                    s.va = std::make_unique<VaSession>(
                        static_cast<VADisplay>(gst_va_display_get_va_dpy(display_)));
                }
                s.ai_geometry = geometry;
                s.va->scale(gst_va_buffer_get_surface(buffer), s.ai_surface, item.frame.width,
                            item.frame.height, item.sw, item.sh);
                auto *output = static_cast<float *>(input->data()) +
                               size_t(valid) * 3 * info.width * info.height;
                pack.pack(*s.image,
                          ImageTransform::make(item.frame.width, item.frame.height, info.width,
                                               info.height),
                          output, {}, stream->get());
                stream->finish();
                const auto period = std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(1.0 / rate));
                const auto admitted = Clock::now();
                // Keep the configured phase; skip missed periods without catch-up bursts.
                s.last_ai = admitted;
                auto &deadline = due[item.channel];
                if (admitted >= deadline)
                    deadline += period * (1 + (admitted - deadline) / period);
                previous[item.slot] = item.frame.sample;
                prepared.push_back(item);
                ++valid;
            }
            if (!valid)
                continue;
            if (valid > 1 && valid < options_.batch) {
                pack.pad(input->data(), valid, options_.batch, info.width, info.height, {},
                         stream->get());
                stream->finish();
            }
            auto executor = valid == 1 && single ? single : batch;
            std::vector<InferenceRequest> requests;
            for (unsigned i = 0; i < valid; i++)
                requests.push_back(
                    {{"images", TensorType::Float32, {1, 3, info.height, info.width}, view(i), 0},
                     prefix + "/" + prepared[i].channel,
                     prepared[i].frame.generation,
                     prepared[i].slot});
            for(const auto &item:prepared)
                if(item.frame.timing.telemetry) item.frame.timing.telemetry->diagnostic(item.frame.timing.frame,"ai_submit");
            const auto infer_started = Clock::now();
            ai_runtime_->record(
                "YOLO preprocess", "",
                std::chrono::duration<double, std::milli>(infer_started - started).count(),
                std::chrono::duration<double, std::milli>(started - waiting_gpu).count(), valid);
            auto pending = executor->submit_many(std::move(requests));
            std::vector<std::vector<Detection>> results(valid);
            for (unsigned i = 0; i < valid; i++)
                for (const auto &object : pending[i].get().objects)
                    results[i].push_back(object.box);
            const auto completed = Clock::now();
            const double elapsed =
                std::chrono::duration<double, std::milli>(completed - started).count();
            ai_runtime_->record(
                "YOLO executor total", "",
                std::chrono::duration<double, std::milli>(completed - infer_started).count(), 0,
                valid);
            batch_ms_ = elapsed;
            last_batch_ = valid;
            for (unsigned i = 0; i < valid; ++i) {
                const auto &item = prepared[i];
                if(item.frame.timing.telemetry) item.frame.timing.telemetry->diagnostic(item.frame.timing.frame,"ai_complete");
                auto &s = ai_slot(item.slot);
                std::lock_guard<std::mutex> l(s.mutex);
                if (s.channel != item.channel || s.generation != item.frame.generation ||
                    (s.latest.sample &&
                     (s.latest.width != item.frame.width || s.latest.height != item.frame.height)))
                    continue;
                s.detections.clear();
                const float sx = float(item.frame.width) / item.sw,
                            sy = float(item.frame.height) / item.sh;
                for (auto box : results[i]) {
                    const float x = std::clamp((box.x - item.px) * sx, 0.f,
                                               float(item.frame.width)),
                                y = std::clamp((box.y - item.py) * sy, 0.f,
                                               float(item.frame.height));
                    const float right = std::clamp((box.x + box.w - item.px) * sx, 0.f,
                                                   float(item.frame.width)),
                                bottom = std::clamp((box.y + box.h - item.py) * sy, 0.f,
                                                    float(item.frame.height));
                    box.x = x;
                    box.y = y;
                    box.w = right - x;
                    box.h = bottom - y;
                    if (box.w > 0 && box.h > 0)
                        s.detections.push_back(box);
                }
                if (search_)
                    search_->submit(item.frame, item.channel, s.detections);
                else {
                    RecordedAnalysis recorded{
                        item.channel,
                        item.frame.recording_session,
                        "yolo",
                        item.frame.wall_time_ms,
                        unsigned(std::min(2000., 2000. / ai_runtime_->fps())),
                        {}};
                    for (auto b : s.detections) {
                        b.x /= item.frame.width;
                        b.w /= item.frame.width;
                        b.y /= item.frame.height;
                        b.h /= item.frame.height;
                        recorded.objects.push_back({b, 0, 0});
                    }
                    catalog_->enqueue_analysis(std::move(recorded));
                }
                if (options_.search.enabled && !options_.search.reid_model.empty())
                    s.detections.erase(std::remove_if(s.detections.begin(), s.detections.end(),
                                                      [](const auto &b) { return b.score < .25f; }),
                                       s.detections.end());
                s.detected_at = item.frame.received;
                s.detection_width = item.frame.width;
                s.detection_height = item.frame.height;
                if (publisher_ && (item.slot >= 16 || !options_.tracking_enabled)) {
                    AnalysisResult result;
                    for (const auto &box : s.detections) {
                        Instance object;
                        object.box = box;
                        result.objects.push_back(object);
                    }
                    auto *metadata = gst_buffer_new();
                    auto *source = gst_sample_get_buffer(item.frame.sample.get());
                    GST_BUFFER_PTS(metadata) = GST_BUFFER_PTS(source);
                    attach_analysis(metadata, std::move(result), batch->info(),
                                    ImageTransform::make(item.frame.width, item.frame.height,
                                                         item.frame.width, item.frame.height,
                                                         false),
                                    {item.channel, item.frame.generation, s.inferred,
                                     GST_BUFFER_PTS(source), item.slot >= 16});
                    publisher_->publish(analysis_json(metadata));
                    gst_buffer_unref(metadata);
                }
                ai_runtime_->record(
                    "Detection update", item.channel, elapsed / valid,
                    std::chrono::duration<double, std::milli>(completed - item.frame.received)
                        .count());
                s.inferred++;
                if (item.slot >= 16) {
                    s.job_done = true;
                    background_cursor = (item.slot - 16 + 1) % background_slots_.size();
                }
            }
            // The requested per-camera rate is explicit; do not increase it with spare GPU time.
            for (const auto &item : prepared)
                if (item.slot < 16)
                    cursor = (item.slot + 1) % 16;
        }
        hip_check(hipDeviceSynchronize(), "AI shutdown");
    } catch (const std::exception &e) {
        std::lock_guard<std::mutex> l(mutex_);
        fatal_ = std::string("AI: ") + e.what();
    }
}
} // namespace amdva::nvr

namespace amdva::nvr {
using namespace amdva::streaming;
PublicationStatus Controller::publication_status() const {
    return publisher_ ? publisher_->status() : PublicationStatus{};
}
} // namespace amdva::nvr

namespace amdva::nvr {
using namespace amdva::streaming;
std::vector<TelemetrySnapshot> Controller::telemetry() const {
    std::vector<std::shared_ptr<Telemetry>> collectors;
    {
        std::lock_guard<std::mutex> lock(telemetry_mutex_);
        for (const auto &entry : telemetry_)
            collectors.push_back(entry.second);
    }
    std::vector<TelemetrySnapshot> snapshots;
    for (const auto &collector : collectors)
        snapshots.push_back(collector->snapshot());
    return snapshots;
}
} // namespace amdva::nvr

namespace amdva::nvr {
using namespace amdva::streaming;
void Controller::presentation_active(bool active) {
    if(active) {
        std::lock_guard<std::mutex> lock(telemetry_mutex_);
        if(!presentation_drop_count_after_)
            presentation_drop_count_after_=Telemetry::monotonic_ns()+10000000000LL;
        for(const auto &entry:telemetry_)
            entry.second->presentation_drop_count_after(presentation_drop_count_after_);
    }
    presentation_observed_ = active;
    for(auto &slot:slots_) {
        const bool requested=options_.preview_enabled && active && slot.preview_visible.load() && !stopping_.load();
        retire_presentation(slot.presentation.active(requested),
            stopping_.load()?"shutdown_fifo_retired":"preview_not_requested",16);
        if(!requested) {
            std::lock_guard<std::mutex> lock(slot.mutex);
            for(auto &plane:slot.preview) {
                if(plane.state!=1 && plane.state!=2 && plane.state!=3 && plane.state!=5)continue;
                if(!plane.presentation_confirmed && !plane.presentation_retired && plane.timing.telemetry)
                    plane.timing.telemetry->diagnostic(plane.timing.frame,
                        stopping_.load()?"shutdown_present_retired":"presentation_suspended_retired");
                plane.presentation_retired=true;
                // Only ready, never sampled surfaces may be freed here. The UI
                // retains current/fenced planes and releases them after its fence.
                if(plane.state==1)plane.state=0;
            }
        }
    }
    const auto states = status();
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    for (const auto &state : states) {
        auto it = telemetry_.find(state.source.id);
        if (it == telemetry_.end())
            continue;
        it->second->availability(TimingStage::PreviewReady, options_.preview_enabled,
                                 options_.preview_enabled && active && state.slot >= 0 &&
                                     slots_[state.slot].preview_visible.load() &&
                                     state.connection == "live");
        it->second->availability(TimingStage::Draw, true,
                                 active && state.slot >= 0 &&
                                     slots_[state.slot].preview_visible.load());
        it->second->availability(TimingStage::Swap, true,
                                 active && state.slot >= 0 &&
                                     slots_[state.slot].preview_visible.load());
    }
}

void Controller::preview_sources(const std::vector<std::string> &ids) {
    preview_selection_.set(ids);
    for(auto &slot:slots_) {
        std::lock_guard<std::mutex> lock(slot.mutex);
        slot.preview_visible=preview_selection_.contains(slot.channel);
    }
    presentation_active(presentation_observed_.load());
}
std::vector<Recording> Controller::recordings(const RecordingQuery &q) const {
    return catalog_->list(q);
}
std::shared_ptr<RecordingFile> Controller::open_recording(const std::string &name) {
    return catalog_->open_recording(name);
}
StorageStatus Controller::storage() const { return catalog_->storage(); }
void Controller::recording_duration(const std::string &name, int64_t ns) {
    catalog_->duration(name, ns);
}
std::vector<std::pair<std::string, std::string>> Controller::recording_cameras() const {
    return catalog_->cameras();
}
// Encoding keeps the video engine busy, so decodes finish later and detection and
// tracking wait for decoded frames ("Decoded frame wait", about 0.15 ms when the
// engine has headroom) while GPU load drops. Live analysis has priority: while that
// wait is long, lower the frame rate every transcoder encodes; raise it again once
// decodes are prompt.
void Controller::pace_transcoding() {
    const bool transcoding = std::any_of(channels_.begin(), channels_.end(), [](const auto &c) {
        return !c->removed && c->transcoder;
    });
    double wait = -1;
    for (const auto &m : ai_runtime_->snapshot())
        if (m.camera.empty() && m.stage == "Decoded frame wait" && m.samples)
            wait = m.mean_ms;
    if (!transcoding || wait < 0)
        return;
    const double full = 30;
    auto &limit = transcode_fps_limit_;
    if (wait > 2)
        limit = std::max(5., (limit > 0 ? limit : full) * .7);
    else if (wait < .5 && limit > 0 && (limit += 2) >= full)
        limit = 0;
    for (auto &c : channels_)
        if (!c->removed && c->transcoder)
            c->transcoder->max_fps(limit);
}
void Controller::update_source(Source source) {
    source.uri = validated_uri(source.uri);
    command([this, source] {
        for (auto &c : channels_)
            if (!c->removed && c->state.source.id == source.id) {
                if (c->state.source.uri != source.uri) {
                    c->failed("Camera configuration changed");
                    c->state.source.uri = source.uri;
                    c->reconnect = Clock::now();
                    c->retries = 0;
                }
                c->state.source.name = source.name;
                for (auto &r : c->recordings)
                    r->camera_name(source.name);
                if (c->state.source.transcode != source.transcode) {
                    // The next keyframe opens a segment with the new settings.
                    c->end_record();
                    c->state.source.transcode = source.transcode;
                    c->reset_output_metrics();
                    c->state.recording = "waiting for keyframe";
                }
                return;
            }
    });
}
} // namespace amdva::nvr

namespace amdva::nvr {
using namespace amdva::streaming;
void Controller::ai_frequency(double fps, unsigned interval) {
    ai_runtime_->configure(fps, interval);
}
} // namespace amdva::nvr
