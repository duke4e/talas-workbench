#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A test-only library loaded with DYLD_INSERT_LIBRARIES. Between begin and
// end it counts, on the calling thread only, the calls a realtime audio
// callback must never make.
enum TalasRealtimeProbeKind
{
    TalasRealtimeProbeAllocation = 0,
    TalasRealtimeProbeDeallocation,
    TalasRealtimeProbeLock,
    TalasRealtimeProbeFileIo,
    TalasRealtimeProbeSleep,
    TalasRealtimeProbeKindCount
};

enum
{
    TalasRealtimeProbeMaximumFrames = 48
};

struct TalasRealtimeProbeReport
{
    uint64_t counts[TalasRealtimeProbeKindCount];
    // The first counted call and the stack that made it.
    const char* firstFunction;
    void* firstFrames[TalasRealtimeProbeMaximumFrames];
    int firstFrameCount;
};

typedef void (*TalasRealtimeProbeBegin)(void);
typedef void (*TalasRealtimeProbeEnd)(struct TalasRealtimeProbeReport* report);

#define TALAS_REALTIME_PROBE_BEGIN_SYMBOL "talas_realtime_probe_begin"
#define TALAS_REALTIME_PROBE_END_SYMBOL "talas_realtime_probe_end"

#ifdef __cplusplus
}
#endif
