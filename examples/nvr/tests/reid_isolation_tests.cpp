#include "backend/controller.hpp"
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <gst/app/gstappsink.h>
#include <gst/va/gstvaallocator.h>
#include <gst/va/gstvadisplay.h>
#include <iostream>
#include <thread>
#include <unistd.h>
using namespace amdva::nvr;
static void check(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
int main(int argc, char **argv) {
    if (argc != 4)
        return 2;
    const auto root = std::filesystem::temp_directory_path() /
                      ("amdva-reid-isolation-" + std::to_string(getpid()));
    try {
        initialize_runtime();
        GError *error{};
        auto *pipeline = gst_parse_launch(
            "filesrc name=file ! h264parse ! vah264dec ! "
            "video/x-raw(memory:VAMemory),format=NV12 ! appsink name=sink sync=false max-buffers=1",
            &error);
        check(!error && pipeline, "Create VA fixture decoder");
        auto *file = gst_bin_get_by_name(GST_BIN(pipeline), "file");
        auto *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
        g_object_set(file, "location", argv[1], nullptr);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        Sample sample(gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 10 * GST_SECOND),
                      gst_sample_unref);
        check(bool(sample), "Decode VA fixture");
        auto *buffer = gst_sample_get_buffer(sample.get());
        auto *display = gst_va_buffer_peek_display(buffer);
        amdva::va_check(vaSyncSurface(static_cast<VADisplay>(gst_va_display_get_va_dpy(display)),
                                      gst_va_buffer_get_surface(buffer)),
                        "Fixture readiness");
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(file);
        gst_object_unref(sink);
        gst_object_unref(pipeline);
        GstVideoInfo video;
        check(gst_video_info_from_caps(&video, gst_sample_get_caps(sample.get())), "Fixture caps");
        for (const std::string phase : {"load", "warmup", "runtime"}) {
            auto directory = root / phase;
            std::filesystem::create_directories(directory);
            std::ofstream(directory / "index.json") << "{\"profiles\":[]}";
            Catalog catalog(directory / "recordings");
            SearchIndex index(catalog);
            SearchOptions options;
            options.enabled = true;
            options.library = phase == "load" ? (directory / "missing.so").string() : argv[3];
            options.bundle = directory.string();
            options.reid_model = argv[2];
            options.image_size = 128;
            auto runtime = std::make_shared<AiRuntime>();
            runtime->configure(4, 3000);
            SearchService service(index, display, options, runtime);
            service.wait_initialized();
            auto state = service.status();
            check(state.tracking_state == "ready" && state.tracking_error.empty(),
                  "ReID did not initialize independently");
            check((phase == "runtime") == (state.state == "ready"),
                  "Embedding fault phase mismatch");
            amdva::Detection person{float(video.width / 4),
                                    float(video.height / 4),
                                    float(video.width / 3),
                                    float(video.height / 2),
                                    .95f,
                                    0};
            uint64_t identity{};
            auto frame_time = int64_t(g_get_real_time() / 1000);
            for (unsigned n = 0; n < 6; ++n) {
                Frame frame;
                frame.sample = sample;
                frame.generation = 1;
                frame.width = video.width;
                frame.height = video.height;
                frame.received = Clock::now();
                frame.wall_time_ms = frame_time + 250 * n;
                frame.recording_session = "isolation";
                service.submit(frame, "camera", {person});
                auto deadline = Clock::now() + std::chrono::seconds(5);
                PersonTrackFrame tracks;
                do {
                    state = service.status();
                    check(state.tracking_error.empty(), "Tracking failed after embedding fault");
                    tracks = service.tracks("camera");
                    if (tracks.captured == frame.received)
                        break;
                    check(Clock::now() < deadline, "Tracking stopped after embedding fault");
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                } while (true);
                if (n) {
                    check(tracks.objects.size() == 1, "Confirmed ReID object disappeared");
                    if (!identity)
                        identity = tracks.objects[0].person;
                    check(identity && tracks.objects[0].person == identity,
                          "ReID identity changed after embedding fault");
                }
                if (phase == "runtime" && n == 1) {
                    const auto deadline = Clock::now() + std::chrono::seconds(5);
                    while (service.status().state != "error") {
                        check(Clock::now() < deadline, "Runtime embedding fault was not reported");
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                }
            }
            state = service.status();
            check(state.state == "error" && !state.error.empty(), "Embedding error was hidden");
            check(state.tracking_state == "ready" && state.tracking_error.empty(),
                  "Embedding failure disabled tracking");
            check(state.pending == 0 && state.stored == 0,
                  "Failed embedding backend accepted more work");
            auto request = service.text("person");
            check(service.result().request == request && !service.result().error.empty(),
                  "Failed embedding query was accepted");
            uint64_t calls{};
            for (auto &m : runtime->snapshot())
                if (m.stage == "ReID inference + transfer" && m.camera.empty())
                    calls = m.completed;
            check(calls == 6, "Real ReID inference did not continue after embedding fault");
            service.shutdown();
            std::cout << phase
                      << ": embedding error visible; 6 ReID updates and stable person ID\n";
        }
        std::filesystem::remove_all(root);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
