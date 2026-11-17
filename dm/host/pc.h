// [pc] port-common runtime report (port-common, README.md "Runtime report"). Every module that decides at run time
// whether it does anything logs exactly one line per process through this, so a player's boot.log says which shared fixes were
// live on that phone:
//   [pc] <id>: applied (<why>)      the fix is active here (e.g. "no LSE: ARMv8.0 thunks")
//   [pc] <id>: skipped (<why>)      the thing it fixes does not exist here (e.g. "CPU has LSE", "DM_VMA_WATCH=0")
// Only the first call per id prints (the rest are no-ops), so a call may sit on a path that runs many times. Safe from any normal
// thread; not from a signal handler (it formats with vsnprintf and logs through LOG).
#pragma once
void pc_note(const char *id, int applied, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
