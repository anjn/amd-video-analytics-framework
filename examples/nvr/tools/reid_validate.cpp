#include "amdva/streaming/preview.hpp"
#include "backend/reid.hpp"
#include <algorithm>
#include <dlfcn.h>
#include <gst/app/gstappsink.h>
#include <gst/va/gstvaallocator.h>
#include <gst/va/gstvadisplay.h>
#include <gst/video/video.h>
#include <iostream>
using namespace amdva::nvr;
int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: amdva-reid-validate MODEL.mxr HEVC.mp4\n";
        return 2;
    }
    try {
        initialize_runtime();
        GError *error{};
        auto *pipeline = gst_parse_launch(
            "filesrc name=file ! qtdemux ! h265parse ! vah265dec ! "
            "video/x-raw(memory:VAMemory),format=NV12 ! appsink name=sink sync=false max-buffers=1",
            &error);
        if (error) {
            std::string text = error->message;
            g_clear_error(&error);
            throw std::runtime_error(text);
        }
        auto *file = gst_bin_get_by_name(GST_BIN(pipeline), "file");
        auto *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
        g_object_set(file, "location", argv[2], nullptr);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        Sample frame(gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 20 * GST_SECOND),
                     gst_sample_unref);
        if (frame) {
            auto *buffer = gst_sample_get_buffer(frame.get());
            auto *display = gst_va_buffer_peek_display(buffer);
            amdva::va_check(
                vaSyncSurface(static_cast<VADisplay>(gst_va_display_get_va_dpy(display)),
                              gst_va_buffer_get_surface(buffer)),
                "Fixture decode completion");
        }
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(file);
        gst_object_unref(sink);
        gst_object_unref(pipeline);
        if (!frame)
            throw std::runtime_error("Missing decoded VA frame");
        GstVideoInfo info;
        if (!gst_video_info_from_caps(&info, gst_sample_get_caps(frame.get())))
            throw std::runtime_error("Missing video caps");
        ReIdModel model(argv[1], 0);
        ReIdInput a{frame,
                    {float(info.width / 4 + 1), float(info.height / 4 + 1), float(info.width / 4),
                     float(info.height / 2), 1, 0}};
        ReIdInput b{frame,
                    {float(info.width / 2), 0, float(info.width / 4), float(info.height), 1, 0}};
        auto first = model.encode(a), other = model.encode(b);
        double different_roi_cosine = reid_similarity(first, other);
        if (different_roi_cosine > .9999)
            throw std::runtime_error("Distinct fixture ROIs produced identical features");
        auto audit = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_start"));
        auto errors =
            reinterpret_cast<unsigned long long (*)()>(dlsym(RTLD_DEFAULT, "amdva_audit_errors"));
        if (!audit || !errors)
            throw std::runtime_error("Load copy auditor with LD_PRELOAD");
        audit();
        double min_cosine = 1;
        const unsigned batch = model.input_shape()[0];
        for (unsigned repeat = 0; repeat < 20; repeat++)
            for (unsigned n : {1u, std::max(1u, batch - 1), batch, batch + 1, 2 * batch + 1}) {
                std::vector<ReIdInput> requests;
                for (unsigned i = 0; i < n; i++)
                    requests.push_back(i % 2 ? b : a);
                auto result = model.encode_many(requests);
                if (result.size() != n)
                    throw std::runtime_error("Padded batch leaked output rows");
                for (unsigned i = 0; i < n; i++) {
                    double c = reid_similarity(result[i], i % 2 ? other : first);
                    min_cosine = std::min(min_cosine, c);
                    if (c < .9999)
                        throw std::runtime_error(
                            "ROI order, ownership or batch parity failed: count=" +
                            std::to_string(n) + " row=" + std::to_string(i) +
                            " cosine=" + std::to_string(c));
                }
            }
        for (unsigned k = 0; k < 100; k++) {
            unsigned w = std::min(100 + 2 * (k % 29), unsigned(info.width / 2)),
                     h = std::min(300 + 2 * (k % 43), unsigned(info.height / 2));
            ReIdInput small{frame,
                            {float((31 * k) % (info.width - w)),
                             float((37 * k) % (info.height - h)), float(w), float(h), 1, 0}};
            model.encode(small);
        }
        if (errors())
            throw std::runtime_error("Copy audit violation");
        bool rejected = false;
        try {
            model.encode({});
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        if (!rejected)
            throw std::runtime_error("Empty ReID frame accepted");
        std::cout
            << "{\"passed\":true,\"preprocessing\":\"amdpreproc\",\"source\":\"NV12 VAMemory "
               "with ROI\","
               "\"input_uploads\":0,\"small_roi_requests\":100,\"encode_requests\":100,\"batch\":"
            << batch << ",\"different_roi_cosine\":" << different_roi_cosine
            << ",\"min_batch_cosine\":" << min_cosine << ",\"audit_errors\":" << errors() << "}\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
