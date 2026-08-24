/*
 * Forgetful — one live input, tape memories that forget themselves.
 *
 * Build step 2 (docs/plans/forgetful-design.md, Build/Test Plan #2): a single
 * LoopEngine (Loop A) in isolation — envelope follower, circular buffer,
 * degradation chain, and the double-click erase trigger. No input routing
 * yet (that's the Master page, Build/Test Plan step 3's job) — this loop
 * always monitors the live input directly.
 *
 * Mixing architecture (see docs/plans/forgetful-design.md, "Signal flow per
 * block"): dry input passes through UNCONDITIONALLY in every state. This is
 * an inline audio_fx sitting between the synth and the output, so silence
 * as *the* output in any non-LOOPING state would mute the whole track.
 * Every state instead contributes an additive WET layer on top of dry —
 * IDLE, RECORDING and FORGOTTEN all contribute zero; only LOOPING's
 * degraded playback is non-zero.
 *
 * Timing model: every timing decision (record debounce, silence timeout,
 * erase confirm window) compares against one shared, monotonically
 * increasing total_frames counter rather than separate ms counters or a
 * wall clock — derived entirely from block size, no syscalls in the hot
 * path.
 *
 * Loop A's page (8 knobs, matches the design doc's per-loop layout minus
 * Volume, which lives on the not-yet-built Master page):
 *
 *   knob 1  loopA_decay_rate   repeats-until-forgotten
 *   knob 2  loopA_wow          wow/flutter
 *   knob 3  loopA_hf_loss      fade to dark (HF loss rate)
 *   knob 4  loopA_hiss         hiss amount
 *   knob 5  loopA_saturation   warmth
 *   knob 6  loopA_chaos        sudden forgetting
 *   knob 7  loopA_reserved     placeholder — structurally required for page
 *                              alignment once Loop B/C/D pages exist;
 *                              read-only dummy per the design doc
 *   knob 8  loopA_erase        access "write" trigger — gesture-test's pattern
 *
 * settings-schema.json (buffer_seconds, record_threshold, silence_timeout,
 * the flavor-knob random/fixed modes, erase_confirm_window_ms) isn't wired
 * yet; the constants below hardcode the design doc's stated defaults.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v2.h"

static const host_api_v1_t *g_host = NULL;

#define SAMPLE_RATE      44100
#define BUFFER_SECONDS   8                              /* settings-schema default */
#define BUFFER_CAPACITY  (SAMPLE_RATE * BUFFER_SECONDS)

#define RECORD_THRESHOLD_DB   -30.0f                     /* settings-schema default */
#define DEBOUNCE_MS           50
#define SILENCE_TIMEOUT_MS    1500
#define MIN_RECORDED_MS       50    /* floor: discard a too-short take rather than loop a click */
#define ERASE_CONFIRM_MS      600
#define FORGOTTEN_DISPLAY_MS  400   /* how long the UI may still report
                                     * "Forgotten" after the engine has
                                     * already reset to IDLE */

#define DEBOUNCE_FRAMES          ((uint64_t)(DEBOUNCE_MS * SAMPLE_RATE / 1000))
#define SILENCE_TIMEOUT_FRAMES   ((uint64_t)(SILENCE_TIMEOUT_MS * SAMPLE_RATE / 1000))
#define MIN_RECORDED_FRAMES      ((int)(MIN_RECORDED_MS * SAMPLE_RATE / 1000))
#define ERASE_CONFIRM_FRAMES     ((uint64_t)(ERASE_CONFIRM_MS * SAMPLE_RATE / 1000))
#define FORGOTTEN_DISPLAY_FRAMES ((uint64_t)(FORGOTTEN_DISPLAY_MS * SAMPLE_RATE / 1000))

#define TIME_NOT_SET UINT64_MAX

#define PI_F 3.14159265358979323846f

#define WOW_RATE_HZ        0.7f
#define FLUTTER_RATE_HZ    7.0f
#define WOW_MOD_DEPTH      0.02f
#define FLUTTER_MOD_DEPTH  0.01f

#define ENV_COEFF  0.05f

#define HISS_CEILING          0.15f
#define SATURATION_MAX_DRIVE  7.0f
#define CHAOS_MAX_PROB        0.0006f  /* per-sample probability ceiling */
#define CHAOS_MUTE_FRAMES     80       /* ~1.8ms glitch length */

/* Flavor randomization ranges (settings-schema "random" mode defaults) */
#define WOW_RAND_MIN         0.1f
#define WOW_RAND_MAX         0.6f
#define HF_LOSS_RAND_MIN     0.2f
#define HF_LOSS_RAND_MAX     0.8f
#define HISS_RAND_MIN        0.0f
#define HISS_RAND_MAX        0.4f
#define SATURATION_RAND_MIN  0.0f
#define SATURATION_RAND_MAX  0.4f
#define CHAOS_RAND_MIN       0.0f
#define CHAOS_RAND_MAX       0.15f

typedef enum {
    LOOP_IDLE = 0,
    LOOP_RECORDING,
    LOOP_LOOPING,
    LOOP_FORGOTTEN
} loop_state_t;

typedef struct {
    int16_t l, r;
} frame16_t;

typedef struct {
    /* state machine + shared time base */
    loop_state_t state;
    uint64_t     total_frames;
    uint64_t     forgotten_at;   /* set the instant memory hits 0; lets the UI
                                  * see "Forgotten" for a short window even
                                  * though `state` has already reset to IDLE */

    /* live knob params (chain_params: loopA_*). decay_rate is never
     * randomized; the other five are re-randomized on RECORDING->LOOPING. */
    float decay_rate;
    float wow;
    float hf_loss;
    float hiss;
    float saturation;
    float chaos;

    /* buffer & playback */
    frame16_t *buffer;
    int        capacity_frames;
    int        write_head;
    int        recorded_length;
    double     read_head;
    float      wow_phase;
    float      flutter_phase;

    /* envelope follower / record detection */
    float    record_threshold_lin;  /* precomputed from RECORD_THRESHOLD_DB */
    float    env_level;
    uint64_t above_threshold_since;
    uint64_t silence_since;

    /* degradation */
    float memory;
    float lp_state_l, lp_state_r;
    int   chaos_mute_remaining;

    /* erase trigger (double-click arm/confirm/timeout) */
    uint64_t erase_armed_since;

    /* noise generator */
    uint32_t rng_state;
} inst_t;

/* ---- small helpers ---- */

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Adds a wet sample (float, roughly -1..1) onto a RAW int16 dry sample and
 * clamps in integer space. Deliberately not dry_float+wet_float->int16: an
 * int16->float->int16 round-trip on the dry term (/32768 then *32767, the
 * same asymmetric convention freeverb.c uses) is off by up to 1 LSB, which
 * would quietly break "dry passes through unconditionally" for any state
 * that contributes wet=0 (IDLE, RECORDING, FORGOTTEN) — bit-exact there is
 * the whole point, not an approximation of it. */
static int16_t mix_dry_wet(int16_t dry, float wet) {
    int32_t out = (int32_t)dry + lroundf(wet * 32767.0f);
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;
    return (int16_t)out;
}

/* xorshift32 — fast, deterministic-enough noise/dice source; not for crypto */
static uint32_t rng_next(uint32_t *seed) {
    uint32_t x = *seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *seed = x;
    return x;
}

/* uniform float in [-1, 1] */
static float rng_bipolar(uint32_t *seed) {
    return ((float)(rng_next(seed) & 0xFFFFFF) / (float)0xFFFFFF) * 2.0f - 1.0f;
}

/* uniform float in [lo, hi] */
static float rng_range(uint32_t *seed, float lo, float hi) {
    float u = (float)(rng_next(seed) & 0xFFFFFF) / (float)0xFFFFFF;
    return lo + u * (hi - lo);
}

/* Clears everything about the current take. Shared by: IDLE->RECORDING entry,
 * discarding a too-short take, FORGOTTEN->IDLE, and a confirmed erase. */
static void reset_take(inst_t *s) {
    s->write_head = 0;
    s->recorded_length = 0;
    s->read_head = 0.0;
    s->above_threshold_since = TIME_NOT_SET;
    s->silence_since = TIME_NOT_SET;
}

/* Fires once on RECORDING->LOOPING. decay_rate is deliberately excluded —
 * it's always the live knob value, never randomized. */
static void randomize_flavor(inst_t *s) {
    s->wow        = rng_range(&s->rng_state, WOW_RAND_MIN, WOW_RAND_MAX);
    s->hf_loss    = rng_range(&s->rng_state, HF_LOSS_RAND_MIN, HF_LOSS_RAND_MAX);
    s->hiss       = rng_range(&s->rng_state, HISS_RAND_MIN, HISS_RAND_MAX);
    s->saturation = rng_range(&s->rng_state, SATURATION_RAND_MIN, SATURATION_RAND_MAX);
    s->chaos      = rng_range(&s->rng_state, CHAOS_RAND_MIN, CHAOS_RAND_MAX);
}

/* ---- instance lifecycle ---- */

static void *v2_create_instance(const char *dir, const char *cfg) {
    (void)dir; (void)cfg;

    inst_t *s = (inst_t *)calloc(1, sizeof(inst_t));
    if (!s) return NULL;

    s->buffer = (frame16_t *)calloc((size_t)BUFFER_CAPACITY, sizeof(frame16_t));
    if (!s->buffer) { free(s); return NULL; }
    s->capacity_frames = BUFFER_CAPACITY;

    s->state = LOOP_IDLE;
    s->decay_rate = 20.0f;
    s->wow        = 0.3f;
    s->hf_loss    = 0.4f;
    s->hiss       = 0.15f;
    s->saturation = 0.15f;
    s->chaos      = 0.05f;

    s->record_threshold_lin = powf(10.0f, RECORD_THRESHOLD_DB / 20.0f);

    s->above_threshold_since = TIME_NOT_SET;
    s->silence_since         = TIME_NOT_SET;
    s->erase_armed_since     = TIME_NOT_SET;
    s->forgotten_at          = TIME_NOT_SET;

    s->rng_state = 0x9E3779B9u; /* fixed seed: deterministic bench-testing */

    return s;
}

static void v2_destroy_instance(void *i) {
    inst_t *s = (inst_t *)i;
    if (!s) return;
    free(s->buffer);
    free(s);
}

/* ---- audio ---- */

static void v2_process_block(void *instance, int16_t *lr, int frames) {
    inst_t *s = (inst_t *)instance;
    if (!s) return;

    for (int i = 0; i < frames; i++) {
        float dry_l = lr[i * 2]     / 32768.0f;
        float dry_r = lr[i * 2 + 1] / 32768.0f;

        /* Envelope follower always runs on the dry input, in every state. */
        float instant = 0.5f * (fabsf(dry_l) + fabsf(dry_r));
        s->env_level += ENV_COEFF * (instant - s->env_level);

        float wet_l = 0.0f, wet_r = 0.0f;

        switch (s->state) {
        case LOOP_IDLE: {
            if (s->env_level > s->record_threshold_lin) {
                if (s->above_threshold_since == TIME_NOT_SET)
                    s->above_threshold_since = s->total_frames;
                if (s->total_frames - s->above_threshold_since >= DEBOUNCE_FRAMES) {
                    s->state = LOOP_RECORDING;
                    reset_take(s);
                    s->forgotten_at = TIME_NOT_SET; /* new activity preempts the display window */
                }
            } else {
                s->above_threshold_since = TIME_NOT_SET;
            }
            break;
        }

        case LOOP_RECORDING: {
            if (s->write_head < s->capacity_frames) {
                s->buffer[s->write_head].l = lr[i * 2];
                s->buffer[s->write_head].r = lr[i * 2 + 1];
                s->write_head++;
            }

            if (s->env_level < s->record_threshold_lin) {
                if (s->silence_since == TIME_NOT_SET)
                    s->silence_since = s->total_frames;
            } else {
                s->silence_since = TIME_NOT_SET;
            }

            int close_silence = (s->silence_since != TIME_NOT_SET) &&
                (s->total_frames - s->silence_since >= SILENCE_TIMEOUT_FRAMES);
            int close_full = (s->write_head >= s->capacity_frames);

            if (close_silence || close_full) {
                if (s->write_head < MIN_RECORDED_FRAMES) {
                    /* too short to be a usable take — discard, don't loop a click */
                    s->state = LOOP_IDLE;
                    reset_take(s);
                } else {
                    s->recorded_length = s->write_head;
                    s->read_head = 0.0;
                    s->memory = 1.0f;
                    s->lp_state_l = s->lp_state_r = 0.0f;
                    s->chaos_mute_remaining = 0;
                    randomize_flavor(s);
                    s->state = LOOP_LOOPING;
                }
            }
            break;
        }

        case LOOP_LOOPING: {
            float degrade = 1.0f - s->memory;

            /* wow/flutter-modulated read speed */
            float mod = (sinf(s->wow_phase) * WOW_MOD_DEPTH +
                         sinf(s->flutter_phase) * FLUTTER_MOD_DEPTH) *
                        s->wow * degrade;
            double speed = 1.0 + mod;

            s->wow_phase += 2.0f * PI_F * WOW_RATE_HZ / SAMPLE_RATE;
            if (s->wow_phase >= 2.0f * PI_F) s->wow_phase -= 2.0f * PI_F;
            s->flutter_phase += 2.0f * PI_F * FLUTTER_RATE_HZ / SAMPLE_RATE;
            if (s->flutter_phase >= 2.0f * PI_F) s->flutter_phase -= 2.0f * PI_F;

            /* interpolated buffer read (recorded_length > 0 is guaranteed
             * while LOOPING — enforced by the MIN_RECORDED_FRAMES floor) */
            int idx0 = (int)floor(s->read_head);
            if (idx0 >= s->recorded_length) idx0 %= s->recorded_length;
            int idx1 = idx0 + 1;
            if (idx1 >= s->recorded_length) idx1 = 0;
            float frac = (float)(s->read_head - floor(s->read_head));

            float raw_l = (s->buffer[idx0].l * (1.0f - frac) + s->buffer[idx1].l * frac) / 32768.0f;
            float raw_r = (s->buffer[idx0].r * (1.0f - frac) + s->buffer[idx1].r * frac) / 32768.0f;

            /* 1. HF-loss LP filter — cutoff closes down as hf_loss * degrade rises.
             *    Perceptual coefficient mapping, not a precise Hz-cutoff design. */
            float filt_amount = clampf(s->hf_loss * degrade, 0.0f, 1.0f);
            float lp_a = 1.0f - filt_amount * 0.95f;
            s->lp_state_l += lp_a * (raw_l - s->lp_state_l);
            s->lp_state_r += lp_a * (raw_r - s->lp_state_r);
            float filt_l = s->lp_state_l, filt_r = s->lp_state_r;

            /* 2. Saturation (Warmth) */
            float sat_amount = clampf(s->saturation * degrade, 0.0f, 1.0f);
            float drive = 1.0f + sat_amount * SATURATION_MAX_DRIVE;
            float sat_l = tanhf(filt_l * drive) / tanhf(drive);
            float sat_r = tanhf(filt_r * drive) / tanhf(drive);

            /* 3. Hiss */
            float hiss_amount = clampf(s->hiss * degrade, 0.0f, 1.0f) * HISS_CEILING;
            float hiss_l = sat_l + rng_bipolar(&s->rng_state) * hiss_amount;
            float hiss_r = sat_r + rng_bipolar(&s->rng_state) * hiss_amount;

            /* 4. Level scaling — this loop's own contribution fades with memory */
            wet_l = hiss_l * s->memory;
            wet_r = hiss_r * s->memory;

            /* 5. Master-page volume — stubbed at unity, no Master page yet */

            /* 6. Chaos gate — occasional brief dropout, applied last, shared
             *    across both channels (a tape glitch, not per-channel noise) */
            if (s->chaos_mute_remaining > 0) {
                s->chaos_mute_remaining--;
                wet_l = wet_r = 0.0f;
            } else {
                float chaos_amount = clampf(s->chaos * degrade, 0.0f, 1.0f);
                float prob = chaos_amount * CHAOS_MAX_PROB;
                if (rng_range(&s->rng_state, 0.0f, 1.0f) < prob) {
                    s->chaos_mute_remaining = CHAOS_MUTE_FRAMES;
                    wet_l = wet_r = 0.0f;
                }
            }

            /* advance + wrap */
            s->read_head += speed;
            if (s->read_head >= s->recorded_length) {
                s->read_head -= s->recorded_length;
                s->memory -= 1.0f / s->decay_rate;
                if (s->memory <= 0.0f) {
                    s->memory = 0.0f;
                    s->state = LOOP_FORGOTTEN;
                }
            }
            break;
        }

        case LOOP_FORGOTTEN: {
            /* Silence flush + drop to IDLE. Audibly this is already
             * guaranteed by LOOPING's own level-scaling (memory == 0 at the
             * transition sample), so collapsing the bookkeeping into the
             * very next sample rather than holding a distinct state for a
             * full 128-frame callback changes nothing a listener can hear —
             * it only affects how soon a new recording can start, and
             * sooner is strictly fine here. forgotten_at records the moment
             * separately so get_param("loopA_status") can still report
             * "Forgotten" for FORGOTTEN_DISPLAY_MS even though `state` is
             * already IDLE by the time any UI poll lands. */
            wet_l = wet_r = 0.0f;
            s->forgotten_at = s->total_frames;
            s->state = LOOP_IDLE;
            reset_take(s);
            s->memory = 0.0f;
            break;
        }
        }

        int16_t raw_dry_l = lr[i * 2];
        int16_t raw_dry_r = lr[i * 2 + 1];
        lr[i * 2]     = mix_dry_wet(raw_dry_l, wet_l);
        lr[i * 2 + 1] = mix_dry_wet(raw_dry_r, wet_r);
    }

    s->total_frames += (uint64_t)frames;
}

/* Simple JSON number extraction (matches freeverb's helper) */
static int json_get_float(const char *json, const char *key, float *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;
    *out = atof(p);
    return 0;
}

/* Display-only state name — distinct from `state` itself: FORGOTTEN reads
 * back here for FORGOTTEN_DISPLAY_MS after the engine has already reset to
 * IDLE, since the engine's own instant reset (LOOP_FORGOTTEN in
 * v2_process_block) would otherwise make FORGOTTEN unobservable to any UI
 * poll landing between process_block calls. Not a chain_params knob — this
 * is groundwork for the per-loop state line (Build/Test Plan step 4),
 * reachable for now only via get_param("loopA_status"). */
static const char *loop_display_state_name(const inst_t *s) {
    if (s->forgotten_at != TIME_NOT_SET &&
        s->total_frames - s->forgotten_at < FORGOTTEN_DISPLAY_FRAMES) {
        return "Forgotten";
    }
    switch (s->state) {
        case LOOP_IDLE:      return "Idle";
        case LOOP_RECORDING: return "Recording";
        case LOOP_LOOPING:   return "Looping";
        case LOOP_FORGOTTEN: return "Forgotten";
    }
    return "Idle";
}

static void v2_set_param(void *inst, const char *key, const char *val) {
    inst_t *s = (inst_t *)inst;
    if (!s || !key || !val) return;

    if (strcmp(key, "state") == 0) {
        float v;
        if (json_get_float(val, "decay_rate", &v) == 0) s->decay_rate = clampf(v, 3.0f, 60.0f);
        if (json_get_float(val, "wow", &v) == 0)         s->wow = clampf(v, 0.0f, 1.0f);
        if (json_get_float(val, "hf_loss", &v) == 0)     s->hf_loss = clampf(v, 0.0f, 1.0f);
        if (json_get_float(val, "hiss", &v) == 0)        s->hiss = clampf(v, 0.0f, 1.0f);
        if (json_get_float(val, "saturation", &v) == 0)  s->saturation = clampf(v, 0.0f, 1.0f);
        if (json_get_float(val, "chaos", &v) == 0)       s->chaos = clampf(v, 0.0f, 1.0f);
        return;
    }

    if (strcmp(key, "loopA_decay_rate") == 0) {
        s->decay_rate = clampf((float)atof(val), 3.0f, 60.0f);
    } else if (strcmp(key, "loopA_wow") == 0) {
        s->wow = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "loopA_hf_loss") == 0) {
        s->hf_loss = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "loopA_hiss") == 0) {
        s->hiss = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "loopA_saturation") == 0) {
        s->saturation = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "loopA_chaos") == 0) {
        s->chaos = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "loopA_erase") == 0) {
        /* gesture-test's pattern: fire on anything that isn't the idle
         * spelling, so an index write of "0" (which MEANS idle) doesn't
         * silently fire it. Double-click-confirm per the design doc. */
        if (strcmp(val, "-") != 0 && strcmp(val, "0") != 0) {
            if (s->erase_armed_since == TIME_NOT_SET) {
                s->erase_armed_since = s->total_frames;
            } else {
                uint64_t elapsed = s->total_frames - s->erase_armed_since;
                if (elapsed <= ERASE_CONFIRM_FRAMES) {
                    /* confirmed: hard clear */
                    s->state = LOOP_IDLE;
                    reset_take(s);
                    s->memory = 0.0f;
                    s->erase_armed_since = TIME_NOT_SET;
                    s->forgotten_at = TIME_NOT_SET; /* explicit erase overrides any stale display */
                } else {
                    /* stale arm — an old click doesn't stay redeemable;
                     * treat this fire as a fresh first press instead */
                    s->erase_armed_since = s->total_frames;
                }
            }
        }
    }
    /* loopA_reserved — access "read" — deliberately unhandled: writing it
     * must do nothing, and the surface must not offer to. */
}

static int v2_get_param(void *inst, const char *key, char *buf, int len) {
    inst_t *s = (inst_t *)inst;
    if (!s || !key || !buf) return -1;

    if (strcmp(key, "name") == 0)              return snprintf(buf, len, "Forgetful");
    if (strcmp(key, "loopA_decay_rate") == 0)  return snprintf(buf, len, "%.1f", s->decay_rate);
    if (strcmp(key, "loopA_wow") == 0)         return snprintf(buf, len, "%.3f", s->wow);
    if (strcmp(key, "loopA_hf_loss") == 0)     return snprintf(buf, len, "%.3f", s->hf_loss);
    if (strcmp(key, "loopA_hiss") == 0)        return snprintf(buf, len, "%.3f", s->hiss);
    if (strcmp(key, "loopA_saturation") == 0)  return snprintf(buf, len, "%.3f", s->saturation);
    if (strcmp(key, "loopA_chaos") == 0)       return snprintf(buf, len, "%.3f", s->chaos);
    if (strcmp(key, "loopA_reserved") == 0)    return snprintf(buf, len, "-");
    if (strcmp(key, "loopA_erase") == 0) {
        return snprintf(buf, len, s->erase_armed_since != TIME_NOT_SET ? "Tap again" : "-");
    }
    if (strcmp(key, "loopA_status") == 0) {
        return snprintf(buf, len, "%s", loop_display_state_name(s));
    }

    if (strcmp(key, "state") == 0) {
        return snprintf(buf, len,
            "{\"decay_rate\":%.4f,\"wow\":%.4f,\"hf_loss\":%.4f,"
            "\"hiss\":%.4f,\"saturation\":%.4f,\"chaos\":%.4f}",
            s->decay_rate, s->wow, s->hf_loss, s->hiss, s->saturation, s->chaos);
    }

    if (strcmp(key, "chain_params") == 0) {
        const char *cp = "["
          "{\"key\":\"loopA_decay_rate\",\"name\":\"Degrade Rate\",\"type\":\"float\","
            "\"min\":3,\"max\":60,\"default\":20,\"step\":1},"
          "{\"key\":\"loopA_wow\",\"name\":\"Wow/Flutter\",\"type\":\"float\","
            "\"min\":0,\"max\":1,\"default\":0.3,\"step\":0.01},"
          "{\"key\":\"loopA_hf_loss\",\"name\":\"Fade to Dark\",\"type\":\"float\","
            "\"min\":0,\"max\":1,\"default\":0.4,\"step\":0.01},"
          "{\"key\":\"loopA_hiss\",\"name\":\"Hiss\",\"type\":\"float\","
            "\"min\":0,\"max\":1,\"default\":0.15,\"step\":0.01},"
          "{\"key\":\"loopA_saturation\",\"name\":\"Warmth\",\"type\":\"float\","
            "\"min\":0,\"max\":1,\"default\":0.15,\"step\":0.01},"
          "{\"key\":\"loopA_chaos\",\"name\":\"Sudden Forgetting\",\"type\":\"float\","
            "\"min\":0,\"max\":1,\"default\":0.05,\"step\":0.01},"
          "{\"key\":\"loopA_reserved\",\"name\":\"-\",\"type\":\"enum\","
            "\"options\":[\"-\"],\"access\":\"read\"},"
          "{\"key\":\"loopA_erase\",\"name\":\"Erase\",\"type\":\"enum\","
            "\"options\":[\"-\",\"Erase!\"],\"access\":\"write\"}"
        "]";
        int n = (int)strlen(cp);
        if (n >= len) return -1;
        strcpy(buf, cp);
        return n;
    }

    if (strcmp(key, "ui_hierarchy") == 0) {
        const char *h = "{"
          "\"modes\":null,"
          "\"levels\":{"
            "\"root\":{"
              "\"label\":\"Loop A\","
              "\"knobs\":[\"loopA_decay_rate\",\"loopA_wow\",\"loopA_hf_loss\","
                         "\"loopA_hiss\",\"loopA_saturation\",\"loopA_chaos\","
                         "\"loopA_reserved\",\"loopA_erase\"]"
            "}"
          "}"
        "}";
        int n = (int)strlen(h);
        if (n >= len) return -1;
        strcpy(buf, h);
        return n;
    }

    return -1;
}

static audio_fx_api_v2_t g_api;

audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    (void)g_host;
    memset(&g_api, 0, sizeof(g_api));
    g_api.api_version      = AUDIO_FX_API_VERSION_2;
    g_api.create_instance  = v2_create_instance;
    g_api.destroy_instance = v2_destroy_instance;
    g_api.process_block    = v2_process_block;
    g_api.set_param        = v2_set_param;
    g_api.get_param        = v2_get_param;
    return &g_api;
}
