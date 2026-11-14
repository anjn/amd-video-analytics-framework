#pragma once
#include <cstdint>
#include <string>
namespace amdva {
// Optional numeric metadata only. No I/O on the calling pipeline thread.
// AMDVA_DIAGNOSTICS_DIR must name a new, dedicated directory for this run.
void diagnostic_event(const std::string &source, uint64_t generation, uint64_t sequence,
                      const char *event, int64_t ns, int64_t value = 0,
                      int64_t value2 = 0, int64_t pts = -1);
bool diagnostics_enabled();
void diagnostics_close(); // Stop after all producers; drain and write loss counters.
} // namespace amdva
