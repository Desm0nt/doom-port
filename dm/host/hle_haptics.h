// Game vibration state (nn::hid VibrationValue per player and motor), before adapting it to a phone or controller motor.
// Portions ported from nxnative (https://github.com/seedds/nxnative) dm/host/hle_haptics.h, Copyright (c) 2026 seedds, MIT License.
#pragma once
#include <stdint.h>

// One player on the host (DOOM is single-player; the npad of player 1 follows the operation mode, hle_hid.c).
#ifndef DM_HAPTICS_PLAYERS
#define DM_HAPTICS_PLAYERS 1
#endif

typedef struct { float amplitude_low, frequency_low, amplitude_high, frequency_high; } dm_vibration;   // = nn::hid::VibrationValue
typedef struct { int player, motor; dm_vibration value; } dm_vibration_update;
typedef struct { int player; uint64_t epoch; dm_vibration motor[2]; } dm_haptic_state;
typedef struct { float intensity, sharpness; } dm_haptic_level;
enum { DM_HAPTICS_INACTIVE = 1, DM_HAPTICS_INTERRUPTED = 2, DM_HAPTICS_BACKGROUND = 4 };

// The sink copies/enqueues each snapshot in order; called under the state lock, it must not reenter.
// No hardware calls on the sending thread. A changed epoch invalidates all older queued snapshots.
void dm_haptics_init(void (*sink)(dm_haptic_state), int enabled);
void dm_haptics_route(int player, int available);   // clears old state even when availability stays the same
void dm_haptics_clear(int player);                 // engine reset: needs a fresh game signal
uint64_t dm_haptics_epoch(int player);
uint32_t dm_haptics_available(void);
int dm_haptics_enabled(void);   // an output sink is installed and rumble is on (whether or not a motor has been found yet)
void dm_haptics_suspend(unsigned reason, int suspended);
void dm_haptics_send(const dm_vibration_update *updates, int count);   // one atomic batch
dm_vibration dm_haptics_actual(int player, int motor);
// Approximate amplitude/frequency as one intensity (0..1) and a sharpness (0 = 40 Hz, 1 = 1250 Hz), for one motor or a mix.
dm_haptic_level dm_haptics_level(const dm_vibration *values, int count, float gain);
// Power of the low band (both Switch motors' amplitude_low) and of the high band, 0..1: for controllers with a heavy and a light motor.
void dm_haptics_bands(const dm_vibration *values, int count, float *low, float *high);
