/*
 * Forgetful — one live input, four tape memories, all of them forgetting.
 *
 * Build step 3 (docs/plans/forgetful-design.md, Build/Test Plan #3):
 * generalizes the single-LoopEngine build (step 2) to all four loops plus
 * the shared input router (Master page). Each loop's own record/degrade/
 * playback behavior is unchanged from step 2 — this step adds:
 *
 *   - a shared `input_routing` selector (None/A/B/C/D): only the currently
 *     routed loop's envelope follower runs at all ("monitors input only if
 *     currently routed"); the other three loops progress on their own
 *     (LOOPING keeps decaying, FORGOTTEN keeps flushing) regardless of
 *     routing.
 *   - routing-change-closes-recording: moving `input_routing` away from a
 *     loop that is currently RECORDING closes it immediately (in
 *     set_param, synchronously), via the same close_recording() path as
 *     the silence-timeout and buffer-full closes.
 *   - dry-once, wet-summed-across-four mixing: dry is read once and added
 *     once per sample (see mix_dry_wet), with all four loops' wet
 *     contributions — each scaled by that loop's own Master-page Volume
 *     knob — summed on top of it first.
 *
 * Mixing architecture (unchanged principle from step 2, docs/plans/
 * forgetful-design.md "Signal flow per block"): dry input passes through
 * UNCONDITIONALLY in every state, for every loop. This is an inline
 * audio_fx sitting between the synth and the output, so silence as *the*
 * output in any non-LOOPING state would mute the whole track.
 *
 * Timing model: unchanged — one shared, monotonically increasing
 * total_frames counter drives every timing decision (record debounce,
 * silence timeout, erase confirm window, forgotten display window) for
 * all four loops. No syscalls in the hot path.
 *
 * Page layout (5 pages x 8 knobs, flat chain_params auto-paginates in
 * declaration order — see CLAUDE.md's "reserved knobs are structurally
 * required" note):
 *
 *   Page 0 (Master):  input_routing, loopA_volume..loopD_volume,
 *                      master_reserved_1..3
 *   Page 1-4 (Loop A/B/C/D): loopX_decay_rate, loopX_wow, loopX_hf_loss,
 *                      loopX_hiss, loopX_saturation, loopX_chaos,
 *                      loopX_reserved, loopX_erase
 *
 * settings-schema.json isn't wired yet; constants below hardcode the
 * design doc's stated defaults, same as step 2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v2.h"

static const host_api_v1_t *g_host = NULL;

#define NUM_LOOPS 4

#define ROUTE_NONE 0
#define ROUTE_A    1
#define ROUTE_B    2
#define ROUTE_C    3
#define ROUTE_D    4

static const char LOOP_LETTERS[NUM_LOOPS] = { 'A', 'B', 'C', 'D' };
static const char *LOOP_PREFIXES[NUM_LOOPS] = { "loopA_", "loopB_", "loopC_", "loopD_" };
static const char *ROUTE_LABELS[NUM_LOOPS + 1] = { "None", "A", "B", "C", "D" };

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

#define DEFAULT_LOOP_VOLUME  0.8f

typedef enum {
    LOOP_IDLE = 0,
    LOOP_RECORDING,
    LOOP_LOOPING,
    LOOP_FORGOTTEN
} loop_state_t;

typedef struct {
    int16_t l, r;
} frame16_t;

/* Everything genuinely per-loop. Timing decisions inside a loop_engine_t
 * compare against the OWNING inst_t's shared total_frames / record_threshold_lin
 * (passed in), not a private copy — all four loops process the same blocks
 * in lockstep, so one shared clock and one shared threshold are correct. */
typedef struct {
    /* state machine */
    loop_state_t state;

    /* live knob params (chain_params: loopX_*). decay_rate is never
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
    float    env_level;
    uint64_t above_threshold_since;
    uint64_t silence_since;

    /* degradation */
    float memory;
    float lp_state_l, lp_state_r;
    int   chaos_mute_remaining;

    /* erase trigger (double-click arm/confirm/timeout) */
    uint64_t erase_armed_since;

    /* display-only: set the instant memory hits 0; lets the UI see
     * "Forgotten" for a short window even though `state` has already
     * reset to IDLE (see loop_display_state_name). */
    uint64_t forgotten_at;

    /* noise generator — independent per loop so hiss/chaos textures and
     * flavor-randomization never perturb each other across loops */
    uint32_t rng_state;
} loop_engine_t;

typedef struct {
    loop_engine_t loops[NUM_LOOPS];

    /* shared time base + shared threshold, both compared against by every
     * loop's timing/record-detection logic */
    uint64_t total_frames;
    float    record_threshold_lin;  /* precomputed from RECORD_THRESHOLD_DB */

    /* Master page */
    int   input_routing;            /* ROUTE_NONE..ROUTE_D */
    float loop_volume[NUM_LOOPS];
} inst_t;

/* ---- small helpers ---- */

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Adds a wet sample (float, roughly -1..1) onto a RAW int16 dry sample and
 * clamps in integer space. Deliberately not dry_float+wet_float->int16: an
 * int16->float->int16 round-trip on the dry term (/32768 then *32767, the
 * same asymmetric convention freeverb.c uses) is off by up to 1 LSB, which
 * would quietly break "dry passes through unconditionally" for any loop
 * that contributes wet=0. Bit-exact there is the whole point, not an
 * approximation of it. */
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
static void reset_take(loop_engine_t *loop) {
    loop->write_head = 0;
    loop->recorded_length = 0;
    loop->read_head = 0.0;
    loop->above_threshold_since = TIME_NOT_SET;
    loop->silence_since = TIME_NOT_SET;
}

/* Fires once on RECORDING->LOOPING. decay_rate is deliberately excluded —
 * it's always the live knob value, never randomized. */
static void randomize_flavor(loop_engine_t *loop) {
    loop->wow        = rng_range(&loop->rng_state, WOW_RAND_MIN, WOW_RAND_MAX);
    loop->hf_loss    = rng_range(&loop->rng_state, HF_LOSS_RAND_MIN, HF_LOSS_RAND_MAX);
    loop->hiss       = rng_range(&loop->rng_state, HISS_RAND_MIN, HISS_RAND_MAX);
    loop->saturation = rng_range(&loop->rng_state, SATURATION_RAND_MIN, SATURATION_RAND_MAX);
    loop->chaos      = rng_range(&loop->rng_state, CHAOS_RAND_MIN, CHAOS_RAND_MAX);
}

/* Shared by all three close triggers: silence-timeout, buffer-full, and a
 * routing change away from this loop (see set_param's "input_routing"). */
static void close_recording(loop_engine_t *loop) {
    if (loop->write_head < MIN_RECORDED_FRAMES) {
        /* too short to be a usable take — discard, don't loop a click */
        loop->state = LOOP_IDLE;
        reset_take(loop);
        return;
    }
    loop->recorded_length = loop->write_head;
    loop->read_head = 0.0;
    loop->memory = 1.0f;
    loop->lp_state_l = loop->lp_state_r = 0.0f;
    loop->chaos_mute_remaining = 0;
    randomize_flavor(loop);
    loop->state = LOOP_LOOPING;
}

/* ---- instance lifecycle ---- */

static void init_loop(loop_engine_t *loop, uint32_t rng_seed) {
    memset(loop, 0, sizeof(*loop));
    loop->decay_rate = 20.0f;
    loop->wow        = 0.3f;
    loop->hf_loss    = 0.4f;
    loop->hiss       = 0.15f;
    loop->saturation = 0.15f;
    loop->chaos      = 0.05f;
    loop->above_threshold_since = TIME_NOT_SET;
    loop->silence_since         = TIME_NOT_SET;
    loop->erase_armed_since     = TIME_NOT_SET;
    loop->forgotten_at          = TIME_NOT_SET;
    loop->rng_state = rng_seed;
}

static void *v2_create_instance(const char *dir, const char *cfg) {
    (void)dir; (void)cfg;

    inst_t *s = (inst_t *)calloc(1, sizeof(inst_t));
    if (!s) return NULL;

    /* distinct, nonzero seeds so each loop's noise/randomization sequence
     * is independent of the others */
    static const uint32_t seeds[NUM_LOOPS] = {
        0x9E3779B9u, 0x85EBCA6Bu, 0xC2B2AE35u, 0x27D4EB2Fu
    };

    for (int i = 0; i < NUM_LOOPS; i++) {
        init_loop(&s->loops[i], seeds[i]);
        s->loops[i].buffer = (frame16_t *)calloc((size_t)BUFFER_CAPACITY, sizeof(frame16_t));
        if (!s->loops[i].buffer) {
            for (int j = 0; j < i; j++) free(s->loops[j].buffer);
            free(s);
            return NULL;
        }
        s->loops[i].capacity_frames = BUFFER_CAPACITY;
        s->loop_volume[i] = DEFAULT_LOOP_VOLUME;
    }

    s->input_routing = ROUTE_NONE;
    s->record_threshold_lin = powf(10.0f, RECORD_THRESHOLD_DB / 20.0f);

    return s;
}

static void v2_destroy_instance(void *i) {
    inst_t *s = (inst_t *)i;
    if (!s) return;
    for (int li = 0; li < NUM_LOOPS; li++) free(s->loops[li].buffer);
    free(s);
}

/* ---- audio ---- */

static void v2_process_block(void *instance, int16_t *lr, int frames) {
    inst_t *s = (inst_t *)instance;
    if (!s) return;

    for (int i = 0; i < frames; i++) {
        int16_t raw_dry_l = lr[i * 2];
        int16_t raw_dry_r = lr[i * 2 + 1];
        float dry_l = raw_dry_l / 32768.0f;
        float dry_r = raw_dry_r / 32768.0f;
        float instant = 0.5f * (fabsf(dry_l) + fabsf(dry_r));

        /* -1 when routing is None; otherwise the loop index (0..3) currently
         * selected by the Master page's Input Routing knob */
        int routed_index = (s->input_routing >= ROUTE_A) ? (s->input_routing - ROUTE_A) : -1;

        float wet_total_l = 0.0f, wet_total_r = 0.0f;

        for (int li = 0; li < NUM_LOOPS; li++) {
            loop_engine_t *loop = &s->loops[li];
            float wet_l = 0.0f, wet_r = 0.0f;

            switch (loop->state) {
            case LOOP_IDLE: {
                /* "monitors input only if currently routed" — an unrouted
                 * IDLE loop does nothing at all this sample. */
                if (li != routed_index) break;

                loop->env_level += ENV_COEFF * (instant - loop->env_level);
                if (loop->env_level > s->record_threshold_lin) {
                    if (loop->above_threshold_since == TIME_NOT_SET)
                        loop->above_threshold_since = s->total_frames;
                    if (s->total_frames - loop->above_threshold_since >= DEBOUNCE_FRAMES) {
                        loop->state = LOOP_RECORDING;
                        reset_take(loop);
                        loop->forgotten_at = TIME_NOT_SET; /* new activity preempts the display window */
                    }
                } else {
                    loop->above_threshold_since = TIME_NOT_SET;
                }
                break;
            }

            case LOOP_RECORDING: {
                /* Invariant: only the routed loop is ever RECORDING — a
                 * routing change away from a RECORDING loop closes it
                 * synchronously in set_param before selection can move
                 * (see the "input_routing" handler below), so this branch
                 * is never reached for an unrouted loop. */
                if (loop->write_head < loop->capacity_frames) {
                    loop->buffer[loop->write_head].l = raw_dry_l;
                    loop->buffer[loop->write_head].r = raw_dry_r;
                    loop->write_head++;
                }

                loop->env_level += ENV_COEFF * (instant - loop->env_level);
                if (loop->env_level < s->record_threshold_lin) {
                    if (loop->silence_since == TIME_NOT_SET)
                        loop->silence_since = s->total_frames;
                } else {
                    loop->silence_since = TIME_NOT_SET;
                }

                int close_silence = (loop->silence_since != TIME_NOT_SET) &&
                    (s->total_frames - loop->silence_since >= SILENCE_TIMEOUT_FRAMES);
                int close_full = (loop->write_head >= loop->capacity_frames);
                if (close_silence || close_full) close_recording(loop);
                break;
            }

            case LOOP_LOOPING: {
                float degrade = 1.0f - loop->memory;

                /* wow/flutter-modulated read speed */
                float mod = (sinf(loop->wow_phase) * WOW_MOD_DEPTH +
                             sinf(loop->flutter_phase) * FLUTTER_MOD_DEPTH) *
                            loop->wow * degrade;
                double speed = 1.0 + mod;

                loop->wow_phase += 2.0f * PI_F * WOW_RATE_HZ / SAMPLE_RATE;
                if (loop->wow_phase >= 2.0f * PI_F) loop->wow_phase -= 2.0f * PI_F;
                loop->flutter_phase += 2.0f * PI_F * FLUTTER_RATE_HZ / SAMPLE_RATE;
                if (loop->flutter_phase >= 2.0f * PI_F) loop->flutter_phase -= 2.0f * PI_F;

                /* interpolated buffer read (recorded_length > 0 is
                 * guaranteed while LOOPING — enforced by the
                 * MIN_RECORDED_FRAMES floor in close_recording) */
                int idx0 = (int)floor(loop->read_head);
                if (idx0 >= loop->recorded_length) idx0 %= loop->recorded_length;
                int idx1 = idx0 + 1;
                if (idx1 >= loop->recorded_length) idx1 = 0;
                float frac = (float)(loop->read_head - floor(loop->read_head));

                float raw_l = (loop->buffer[idx0].l * (1.0f - frac) + loop->buffer[idx1].l * frac) / 32768.0f;
                float raw_r = (loop->buffer[idx0].r * (1.0f - frac) + loop->buffer[idx1].r * frac) / 32768.0f;

                /* 1. HF-loss LP filter — perceptual coefficient mapping,
                 *    not a precise Hz-cutoff design */
                float filt_amount = clampf(loop->hf_loss * degrade, 0.0f, 1.0f);
                float lp_a = 1.0f - filt_amount * 0.95f;
                loop->lp_state_l += lp_a * (raw_l - loop->lp_state_l);
                loop->lp_state_r += lp_a * (raw_r - loop->lp_state_r);
                float filt_l = loop->lp_state_l, filt_r = loop->lp_state_r;

                /* 2. Saturation (Warmth) */
                float sat_amount = clampf(loop->saturation * degrade, 0.0f, 1.0f);
                float drive = 1.0f + sat_amount * SATURATION_MAX_DRIVE;
                float sat_l = tanhf(filt_l * drive) / tanhf(drive);
                float sat_r = tanhf(filt_r * drive) / tanhf(drive);

                /* 3. Hiss */
                float hiss_amount = clampf(loop->hiss * degrade, 0.0f, 1.0f) * HISS_CEILING;
                float hiss_l = sat_l + rng_bipolar(&loop->rng_state) * hiss_amount;
                float hiss_r = sat_r + rng_bipolar(&loop->rng_state) * hiss_amount;

                /* 4. Level scaling — this loop's own contribution fades with memory */
                wet_l = hiss_l * loop->memory;
                wet_r = hiss_r * loop->memory;

                /* 5. Master-page volume is applied once, after summing all
                 * four loops' wet below — not here per-loop-in-isolation,
                 * since it's the same multiply either way and this keeps
                 * the per-loop block symmetric with step 2. */

                /* 6. Chaos gate — occasional brief dropout, applied last,
                 *    shared across both channels (a tape glitch, not
                 *    per-channel noise) */
                if (loop->chaos_mute_remaining > 0) {
                    loop->chaos_mute_remaining--;
                    wet_l = wet_r = 0.0f;
                } else {
                    float chaos_amount = clampf(loop->chaos * degrade, 0.0f, 1.0f);
                    float prob = chaos_amount * CHAOS_MAX_PROB;
                    if (rng_range(&loop->rng_state, 0.0f, 1.0f) < prob) {
                        loop->chaos_mute_remaining = CHAOS_MUTE_FRAMES;
                        wet_l = wet_r = 0.0f;
                    }
                }

                /* advance + wrap */
                loop->read_head += speed;
                if (loop->read_head >= loop->recorded_length) {
                    loop->read_head -= loop->recorded_length;
                    loop->memory -= 1.0f / loop->decay_rate;
                    if (loop->memory <= 0.0f) {
                        loop->memory = 0.0f;
                        loop->state = LOOP_FORGOTTEN;
                    }
                }
                break;
            }

            case LOOP_FORGOTTEN: {
                /* Silence flush + drop to IDLE, regardless of routing —
                 * "decay progresses regardless of routing" applies to
                 * forgetting too. See step 2's comment: collapsing this
                 * into the very next sample rather than holding a distinct
                 * state for a full callback is audibly identical, since
                 * LOOPING's own level-scaling already guarantees silence
                 * at the transition sample. */
                wet_l = wet_r = 0.0f;
                loop->forgotten_at = s->total_frames;
                loop->state = LOOP_IDLE;
                reset_take(loop);
                loop->memory = 0.0f;
                break;
            }
            }

            wet_total_l += wet_l * s->loop_volume[li];
            wet_total_r += wet_r * s->loop_volume[li];
        }

        lr[i * 2]     = mix_dry_wet(raw_dry_l, wet_total_l);
        lr[i * 2 + 1] = mix_dry_wet(raw_dry_r, wet_total_r);
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
 * IDLE, since the engine's own instant reset (LOOP_FORGOTTEN above) would
 * otherwise make FORGOTTEN unobservable to any UI poll landing between
 * process_block calls. Not a chain_params knob — this is groundwork for
 * the per-loop state line (Build/Test Plan step 4), reachable for now only
 * via get_param("loopX_status"). */
/* total_frames lives on inst_t, not loop_engine_t, but the comparison only
 * needs the DELTA since forgotten_at (itself stamped from that same shared
 * counter) — so the caller passes the current total_frames in rather than
 * this function reaching for it. */
static const char *loop_display_state_name(const loop_engine_t *loop, uint64_t total_frames) {
    if (loop->forgotten_at != TIME_NOT_SET &&
        total_frames - loop->forgotten_at < FORGOTTEN_DISPLAY_FRAMES) {
        return "Forgotten";
    }
    switch (loop->state) {
        case LOOP_IDLE:      return "Idle";
        case LOOP_RECORDING: return "Recording";
        case LOOP_LOOPING:   return "Looping";
        case LOOP_FORGOTTEN: return "Forgotten";
    }
    return "Idle";
}

/* Returns the loop index (0..3) and the suffix after "loopX_" if `key`
 * names a per-loop param, or NULL if `key` isn't one (including Master-page
 * keys that merely start with "loop" — see the "loopX_volume" note below). */
static const char *loop_key_suffix(const char *key, int *loop_index_out) {
    for (int i = 0; i < NUM_LOOPS; i++) {
        size_t plen = strlen(LOOP_PREFIXES[i]);
        if (strncmp(key, LOOP_PREFIXES[i], plen) == 0) {
            *loop_index_out = i;
            return key + plen;
        }
    }
    return NULL;
}

static void loop_set_param(loop_engine_t *loop, uint64_t total_frames, const char *suffix, const char *val) {
    if (strcmp(suffix, "decay_rate") == 0) {
        loop->decay_rate = clampf((float)atof(val), 3.0f, 60.0f);
    } else if (strcmp(suffix, "wow") == 0) {
        loop->wow = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(suffix, "hf_loss") == 0) {
        loop->hf_loss = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(suffix, "hiss") == 0) {
        loop->hiss = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(suffix, "saturation") == 0) {
        loop->saturation = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(suffix, "chaos") == 0) {
        loop->chaos = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(suffix, "erase") == 0) {
        /* gesture-test's pattern: fire on anything that isn't the idle
         * spelling. Double-click-confirm per the design doc. */
        if (strcmp(val, "-") != 0 && strcmp(val, "0") != 0) {
            if (loop->erase_armed_since == TIME_NOT_SET) {
                loop->erase_armed_since = total_frames;
            } else {
                uint64_t elapsed = total_frames - loop->erase_armed_since;
                if (elapsed <= ERASE_CONFIRM_FRAMES) {
                    loop->state = LOOP_IDLE;
                    reset_take(loop);
                    loop->memory = 0.0f;
                    loop->erase_armed_since = TIME_NOT_SET;
                    loop->forgotten_at = TIME_NOT_SET;
                } else {
                    /* stale arm — an old click doesn't stay redeemable */
                    loop->erase_armed_since = total_frames;
                }
            }
        }
    }
    /* "reserved" — access "read" — deliberately unhandled: writing it must
     * do nothing, and the surface must not offer to. */
}

static int loop_get_param(const loop_engine_t *loop, uint64_t total_frames, const char *suffix, char *buf, int len) {
    if (strcmp(suffix, "decay_rate") == 0)  return snprintf(buf, len, "%.1f", loop->decay_rate);
    if (strcmp(suffix, "wow") == 0)         return snprintf(buf, len, "%.3f", loop->wow);
    if (strcmp(suffix, "hf_loss") == 0)     return snprintf(buf, len, "%.3f", loop->hf_loss);
    if (strcmp(suffix, "hiss") == 0)        return snprintf(buf, len, "%.3f", loop->hiss);
    if (strcmp(suffix, "saturation") == 0)  return snprintf(buf, len, "%.3f", loop->saturation);
    if (strcmp(suffix, "chaos") == 0)       return snprintf(buf, len, "%.3f", loop->chaos);
    if (strcmp(suffix, "reserved") == 0)    return snprintf(buf, len, "-");
    if (strcmp(suffix, "erase") == 0) {
        return snprintf(buf, len, loop->erase_armed_since != TIME_NOT_SET ? "Tap again" : "-");
    }
    if (strcmp(suffix, "status") == 0) {
        return snprintf(buf, len, "%s", loop_display_state_name(loop, total_frames));
    }
    return -1;
}

static void v2_set_param(void *inst, const char *key, const char *val) {
    inst_t *s = (inst_t *)inst;
    if (!s || !key || !val) return;

    if (strcmp(key, "state") == 0) {
        float v;
        if (json_get_float(val, "input_routing", &v) == 0) {
            int r = (int)v;
            if (r >= ROUTE_NONE && r <= ROUTE_D) s->input_routing = r;
            /* A freshly-restored instance has nothing RECORDING yet, so
             * there's no close-on-change side effect to run here. */
        }
        for (int i = 0; i < NUM_LOOPS; i++) {
            char key_buf[32];
            snprintf(key_buf, sizeof(key_buf), "loop%c_volume", LOOP_LETTERS[i]);
            if (json_get_float(val, key_buf, &v) == 0) s->loop_volume[i] = clampf(v, 0.0f, 1.0f);

            snprintf(key_buf, sizeof(key_buf), "loop%c_decay_rate", LOOP_LETTERS[i]);
            if (json_get_float(val, key_buf, &v) == 0) s->loops[i].decay_rate = clampf(v, 3.0f, 60.0f);
            snprintf(key_buf, sizeof(key_buf), "loop%c_wow", LOOP_LETTERS[i]);
            if (json_get_float(val, key_buf, &v) == 0) s->loops[i].wow = clampf(v, 0.0f, 1.0f);
            snprintf(key_buf, sizeof(key_buf), "loop%c_hf_loss", LOOP_LETTERS[i]);
            if (json_get_float(val, key_buf, &v) == 0) s->loops[i].hf_loss = clampf(v, 0.0f, 1.0f);
            snprintf(key_buf, sizeof(key_buf), "loop%c_hiss", LOOP_LETTERS[i]);
            if (json_get_float(val, key_buf, &v) == 0) s->loops[i].hiss = clampf(v, 0.0f, 1.0f);
            snprintf(key_buf, sizeof(key_buf), "loop%c_saturation", LOOP_LETTERS[i]);
            if (json_get_float(val, key_buf, &v) == 0) s->loops[i].saturation = clampf(v, 0.0f, 1.0f);
            snprintf(key_buf, sizeof(key_buf), "loop%c_chaos", LOOP_LETTERS[i]);
            if (json_get_float(val, key_buf, &v) == 0) s->loops[i].chaos = clampf(v, 0.0f, 1.0f);
        }
        return;
    }

    /* Master page — checked by exact key BEFORE the loopX_ prefix scan,
     * since "loopA_volume" etc. share the "loopA_" prefix with Loop A's
     * own params but are actually Master-page keys. */
    if (strcmp(key, "input_routing") == 0) {
        int new_route = atoi(val);
        if (new_route >= ROUTE_NONE && new_route <= ROUTE_D && new_route != s->input_routing) {
            if (s->input_routing >= ROUTE_A) {
                loop_engine_t *old_loop = &s->loops[s->input_routing - ROUTE_A];
                if (old_loop->state == LOOP_RECORDING) {
                    /* "moving on is treated as I'm done with this one" —
                     * the same close path silence-timeout/buffer-full use */
                    close_recording(old_loop);
                }
            }
            s->input_routing = new_route;
        }
        return;
    }
    if (strcmp(key, "loopA_volume") == 0) { s->loop_volume[0] = clampf((float)atof(val), 0.0f, 1.0f); return; }
    if (strcmp(key, "loopB_volume") == 0) { s->loop_volume[1] = clampf((float)atof(val), 0.0f, 1.0f); return; }
    if (strcmp(key, "loopC_volume") == 0) { s->loop_volume[2] = clampf((float)atof(val), 0.0f, 1.0f); return; }
    if (strcmp(key, "loopD_volume") == 0) { s->loop_volume[3] = clampf((float)atof(val), 0.0f, 1.0f); return; }
    /* master_reserved_1..3 — access "read" — deliberately unhandled */

    int li;
    const char *suffix = loop_key_suffix(key, &li);
    if (suffix) loop_set_param(&s->loops[li], s->total_frames, suffix, val);
}

static int v2_get_param(void *inst, const char *key, char *buf, int len) {
    inst_t *s = (inst_t *)inst;
    if (!s || !key || !buf) return -1;

    if (strcmp(key, "name") == 0) return snprintf(buf, len, "Forgetful");

    /* Master page — same exact-match-before-prefix-scan ordering as set_param */
    if (strcmp(key, "input_routing") == 0) return snprintf(buf, len, "%s", ROUTE_LABELS[s->input_routing]);
    if (strcmp(key, "loopA_volume") == 0)  return snprintf(buf, len, "%.3f", s->loop_volume[0]);
    if (strcmp(key, "loopB_volume") == 0)  return snprintf(buf, len, "%.3f", s->loop_volume[1]);
    if (strcmp(key, "loopC_volume") == 0)  return snprintf(buf, len, "%.3f", s->loop_volume[2]);
    if (strcmp(key, "loopD_volume") == 0)  return snprintf(buf, len, "%.3f", s->loop_volume[3]);
    if (strcmp(key, "master_reserved_1") == 0 ||
        strcmp(key, "master_reserved_2") == 0 ||
        strcmp(key, "master_reserved_3") == 0) return snprintf(buf, len, "-");

    int li;
    const char *suffix = loop_key_suffix(key, &li);
    if (suffix) return loop_get_param(&s->loops[li], s->total_frames, suffix, buf, len);

    if (strcmp(key, "state") == 0) {
        char json[1024];
        int pos = snprintf(json, sizeof(json), "{\"input_routing\":%d", s->input_routing);
        for (int i = 0; i < NUM_LOOPS; i++) {
            pos += snprintf(json + pos, sizeof(json) - pos, ",\"loop%c_volume\":%.4f",
                             LOOP_LETTERS[i], s->loop_volume[i]);
        }
        for (int i = 0; i < NUM_LOOPS; i++) {
            const loop_engine_t *loop = &s->loops[i];
            pos += snprintf(json + pos, sizeof(json) - pos,
                ",\"loop%c_decay_rate\":%.4f,\"loop%c_wow\":%.4f,\"loop%c_hf_loss\":%.4f,"
                "\"loop%c_hiss\":%.4f,\"loop%c_saturation\":%.4f,\"loop%c_chaos\":%.4f",
                LOOP_LETTERS[i], loop->decay_rate, LOOP_LETTERS[i], loop->wow,
                LOOP_LETTERS[i], loop->hf_loss, LOOP_LETTERS[i], loop->hiss,
                LOOP_LETTERS[i], loop->saturation, LOOP_LETTERS[i], loop->chaos);
        }
        pos += snprintf(json + pos, sizeof(json) - pos, "}");
        if (pos >= (int)sizeof(json) || pos >= len) return -1;
        strcpy(buf, json);
        return pos;
    }

    if (strcmp(key, "chain_params") == 0) {
        char json[8192];
        int pos = snprintf(json, sizeof(json), "[");
        pos += snprintf(json + pos, sizeof(json) - pos,
            "{\"key\":\"input_routing\",\"name\":\"Input Routing\",\"type\":\"enum\","
              "\"options\":[\"None\",\"A\",\"B\",\"C\",\"D\"],\"default\":0}");
        for (int i = 0; i < NUM_LOOPS; i++) {
            pos += snprintf(json + pos, sizeof(json) - pos,
                ",{\"key\":\"loop%c_volume\",\"name\":\"Vol %c\",\"type\":\"float\","
                  "\"min\":0,\"max\":1,\"default\":%.2f,\"step\":0.01,\"unit\":\"%%\","
                  "\"display_format\":\"%%.0f\"}",
                LOOP_LETTERS[i], LOOP_LETTERS[i], (double)DEFAULT_LOOP_VOLUME);
        }
        for (int i = 0; i < 3; i++) {
            pos += snprintf(json + pos, sizeof(json) - pos,
                ",{\"key\":\"master_reserved_%d\",\"name\":\"-\",\"type\":\"enum\","
                  "\"options\":[\"-\"],\"access\":\"read\"}", i + 1);
        }
        for (int i = 0; i < NUM_LOOPS; i++) {
            char c = LOOP_LETTERS[i];
            pos += snprintf(json + pos, sizeof(json) - pos,
                ",{\"key\":\"loop%c_decay_rate\",\"name\":\"Degrade Rate\",\"type\":\"float\","
                  "\"min\":3,\"max\":60,\"default\":20,\"step\":1}"
                ",{\"key\":\"loop%c_wow\",\"name\":\"Wow/Flutter\",\"type\":\"float\","
                  "\"min\":0,\"max\":1,\"default\":0.3,\"step\":0.01}"
                ",{\"key\":\"loop%c_hf_loss\",\"name\":\"Fade to Dark\",\"type\":\"float\","
                  "\"min\":0,\"max\":1,\"default\":0.4,\"step\":0.01}"
                ",{\"key\":\"loop%c_hiss\",\"name\":\"Hiss\",\"type\":\"float\","
                  "\"min\":0,\"max\":1,\"default\":0.15,\"step\":0.01}"
                ",{\"key\":\"loop%c_saturation\",\"name\":\"Warmth\",\"type\":\"float\","
                  "\"min\":0,\"max\":1,\"default\":0.15,\"step\":0.01}"
                ",{\"key\":\"loop%c_chaos\",\"name\":\"Sudden Forgetting\",\"type\":\"float\","
                  "\"min\":0,\"max\":1,\"default\":0.05,\"step\":0.01}"
                ",{\"key\":\"loop%c_reserved\",\"name\":\"-\",\"type\":\"enum\","
                  "\"options\":[\"-\"],\"access\":\"read\"}"
                ",{\"key\":\"loop%c_erase\",\"name\":\"Erase\",\"type\":\"enum\","
                  "\"options\":[\"-\",\"Erase!\"],\"access\":\"write\"}",
                c, c, c, c, c, c, c, c);
        }
        pos += snprintf(json + pos, sizeof(json) - pos, "]");
        if (pos >= (int)sizeof(json) || pos >= len) return -1;
        strcpy(buf, json);
        return pos;
    }

    if (strcmp(key, "ui_hierarchy") == 0) {
        char json[2048];
        int pos = snprintf(json, sizeof(json),
            "{\"modes\":null,\"levels\":{\"root\":{\"label\":\"Forgetful\",\"knobs\":["
            "\"input_routing\",\"loopA_volume\",\"loopB_volume\",\"loopC_volume\",\"loopD_volume\","
            "\"master_reserved_1\",\"master_reserved_2\",\"master_reserved_3\"");
        for (int i = 0; i < NUM_LOOPS; i++) {
            char c = LOOP_LETTERS[i];
            pos += snprintf(json + pos, sizeof(json) - pos,
                ",\"loop%c_decay_rate\",\"loop%c_wow\",\"loop%c_hf_loss\",\"loop%c_hiss\","
                "\"loop%c_saturation\",\"loop%c_chaos\",\"loop%c_reserved\",\"loop%c_erase\"",
                c, c, c, c, c, c, c, c);
        }
        pos += snprintf(json + pos, sizeof(json) - pos, "]}}}");
        if (pos >= (int)sizeof(json) || pos >= len) return -1;
        strcpy(buf, json);
        return pos;
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
