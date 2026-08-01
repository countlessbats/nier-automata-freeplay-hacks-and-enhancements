#pragma once

bool install_timescale_hook();

// Returns false when the request was suppressed because the previous hitstop
// started less than `minimum_interval_ms` ago.
bool begin_hitstop(float scale, unsigned duration_ms, unsigned minimum_interval_ms);

void reset_timescale();
