// Included by N64Recomp's output when recomp/swrs.toml has trace_mode = true.
// Bring-up aid: counts calls per game function while switched on (see swrs_game.cpp:
// create C:\projects\swrs-trace-now to record 2 seconds into C:\projects\swrs-trace.txt).
#ifndef SWRS_TRACE_H
#define SWRS_TRACE_H

#ifdef __cplusplus
extern "C" {
#endif

extern volatile int swrs_trace_on;
void swrs_trace_hit(const char* name);

#ifdef __cplusplus
}
#endif

#define TRACE_ENTRY() do { if (swrs_trace_on) { swrs_trace_hit(__func__); } } while (0);
#define TRACE_RETURN()

#endif
