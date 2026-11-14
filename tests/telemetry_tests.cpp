#include "amdva/telemetry.hpp"
#include "../src/diagnostic_writer.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <iostream>
#include <stdexcept>
using namespace amdva;
void check(bool v, const char *s) {
    if (!v)
        throw std::runtime_error(s);
}
int main() {
    try {
        const auto temp=std::filesystem::temp_directory_path()/ ("amdva-diagnostics-"+std::to_string(getpid()));
        auto read=[](const auto &p){std::ifstream in(p); return std::string(std::istreambuf_iterator<char>(in),{});};
        {
            detail::DiagnosticWriter writer(temp/"normal",128);
            detail::DiagnosticRecord r;
            std::strcpy(r.source.data(),"cam0"); std::strcpy(r.event.data(),"decode_output");
            r.generation=7; r.sequence=9; r.ns=200; r.pts=900;
            writer.push(r); r.ns=100; r.sequence=10; writer.push(r);
            writer.close();
            const auto data=read(temp/"normal/events.csv");
            check(data.find("200,\"cam0\",7,9")!=std::string::npos && data.find("100,\"cam0\",7,10")!=std::string::npos,"Preserve identities, PTS and clock regression in raw diagnostics");
            check(read(temp/"normal/diagnostics-summary.json").find("\"written\":2")!=std::string::npos,"Drain diagnostic queue on close");
        }
        bool reused=false;
        try {detail::DiagnosticWriter writer(temp/"normal");}catch(const std::exception&){reused=true;}
        check(reused,"Never overwrite previous diagnostic evidence");
        {
            detail::DiagnosticWriter writer(temp/"limit",128,0);
            writer.push({}); writer.close();
            const auto summary=read(temp/"limit/diagnostics-summary.json");
            check(summary.find("\"limit_lost\":1")!=std::string::npos,"Report file capacity loss independently of frame loss");
        }
        {
            detail::DiagnosticWriter writer(temp/"overflow",1,1024);
            for(unsigned i=0;i<100000;++i)writer.push({});
            writer.close();
            check(read(temp/"overflow/diagnostics-summary.json").find("\"queue_lost\":0")==std::string::npos,"Report bounded queue overflow");
        }
        std::filesystem::remove_all(temp);

        int64_t at = 1000000000LL;
        auto t = std::make_shared<Telemetry>("camera\"1", TelemetryOptions{}, [&] { return at; });
        t->begin_generation(1);
        for (unsigned i = 0; i < timing_stage_count; i++)
            t->availability(TimingStage(i), true, true);
        auto f = t->frame(at);
        auto copy = f;
        at += 20000000;
        check(t->mark(f, TimingStage::AccessUnit), "AU");
        at += 10000000;
        t->mark(f, TimingStage::DecodeInput);
        at += 30000000;
        t->mark(f, TimingStage::DecodeOutput);
        at += 10000000;
        t->mark(f, TimingStage::PreviewReady);
        at += 10000000;
        t->mark(f, TimingStage::Draw);
        at += 16000000;
        t->mark(f, TimingStage::Swap);
        check(!t->mark(f, TimingStage::Swap), "Duplicate trace");
        check(!t->mark(copy, TimingStage::Swap), "Duplicate copy");
        check(t->snapshot().invalid_time == 0, "Repaint is not invalid stage ordering");
        auto s = t->snapshot();
        check(s.latency[0][0].mean_ms == 60, "Input/decode");
        check(s.latency[0][1].mean_ms == 30, "Decoder");
        check(s.latency[0][3].mean_ms == 96, "Input/swap");
        check(s.stages[0][6].count == 1, "Count");
        check(s.json().find("camera\\\"1") != std::string::npos, "JSON escaping");
        at += 1200000000;
        s = t->snapshot();
        check(s.stages[0][6].stalled && s.stages[0][6].stops == 1, "Stop begins");
        check(t->snapshot().stages[0][6].stops == 1, "Stop counted once");
        auto next = t->frame(at);
        t->mark(next, TimingStage::Swap);
        s = t->snapshot();
        check(!s.stages[0][6].stalled && s.stages[0][6].stopped_ms >= 1200, "Stop closes");
        t->availability(TimingStage::Swap, true, false);
        at += 2000000000;
        check(!t->snapshot().stages[0][6].stalled, "Pause");
        auto bg = t->frame(at, true);
        t->mark(bg, TimingStage::DecodeOutput);
        at += 2000000000;
        check(!t->snapshot().stages[1][3].stalled, "Sparse background");
        t->begin_generation(2);
        check(!t->mark(f, TimingStage::Swap), "Old generation");
        check(t->snapshot().stale == 1, "Stale counter");
        auto invalid = t->frame(at);
        check(!t->mark(invalid, TimingStage::DecodeOutput, at - 1), "Negative interval");
        for (unsigned i = 0; i < 4200; i++) {
            at += 1000;
            auto frame = t->frame(at);
            at += 1000;
            t->mark(frame, TimingStage::DecodeOutput);
        }
        s = t->snapshot();
        check(s.latency[0][0].samples == 4096 && s.latency[0][0].truncated, "Bounded samples");
        at += 11000000000LL;
        check(t->snapshot().latency[0][0].samples == 0, "Window expiry");
        auto shared = t->frame(at);
        auto draw = shared, swap = shared;
        at += 1000000;
        t->mark(draw, TimingStage::Draw);
        at += 2000000;
        t->mark(swap, TimingStage::Swap);
        check(swap.ns[unsigned(TimingStage::Draw)] == draw.ns[unsigned(TimingStage::Draw)],
              "Copies recover canonical ancestor timestamps");
        check(t->snapshot().latency[0][6].mean_ms == 2, "Cross-copy draw/swap interval");
        Telemetry exact("exact", {}, [&] { return at; });
        exact.begin_generation();
        exact.availability(TimingStage::DecodeOutput, true, true);
        check(exact.snapshot().stages[0][3].age_ms == -1, "No invented first observation age");
        for (unsigned i = 1; i <= 100; i++) {
            auto frame = exact.frame(at);
            at += i * 1000000LL;
            exact.mark(frame, TimingStage::DecodeOutput);
        }
        auto distribution = exact.snapshot().latency[0][0];
        check(distribution.samples == 100 && distribution.mean_ms == 50.5 &&
                  distribution.p95_ms == 95 && distribution.p99_ms == 99 &&
                  distribution.maximum_ms == 100,
              "Exact nearest-rank percentiles and mean");
        at += 1200000000LL;
        exact.activity(TimingStage::DecodeOutput, false);
        auto paused = exact.snapshot().stages[0][3];
        check(paused.stops == 1 && paused.stopped_ms == 1200 && !paused.stalled,
              "Pause accounts gap without intermediate snapshots");
        exact.activity(TimingStage::DecodeOutput, true);
        auto restarted = exact.frame(at);
        exact.mark(restarted, TimingStage::DecodeOutput);
        at += 1300000000LL;
        exact.begin_generation();
        check(exact.snapshot().stages[0][3].stops == 2 &&
                  exact.snapshot().stages[0][3].stopped_ms == 2500,
              "Generation retirement accounts final observed gap");
        Telemetry disabled("off", {false, 1000}, [&] { return at; });
        auto empty = disabled.frame(at);
        check(!disabled.mark(empty, TimingStage::Draw) && !disabled.snapshot().enabled, "Disabled");
        std::cout << "Telemetry interval, identity, stop, bounds and disable contracts passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
