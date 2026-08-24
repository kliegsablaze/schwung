/*
 * Bench test for Forgetful's LoopEngine (docs/plans/forgetful-design.md,
 * Build/Test Plan #5): synthetic tone/silence input driven straight through
 * the module's public v2 API (create_instance/set_param/get_param/
 * process_block), exactly as chain_host would drive it. Black-box — this
 * does not reach into forgetful.c's internals, so the timing constants below
 * mirror the design doc's documented defaults rather than forgetful.c's
 * private macros.
 *
 * Covers:
 *   0. chain_params / ui_hierarchy shape — exactly 40 entries (5 pages x 8),
 *      every expected key present in both.
 *   1. record trigger — debounce timing, and dry input passing through
 *      unconditionally (IDLE and RECORDING both). Loop A only; the four
 *      loops' own per-loop behavior is identical, so this isn't repeated
 *      per letter.
 *   2. decay timing — memory reaching 0 after decay_rate wraps, and the
 *      forgotten_at display window (get_param("loopX_status") reporting
 *      "Forgotten" for a while after the engine has already reset to Idle).
 *   3. double-click / stale-rearm erase — a single click arms but doesn't
 *      clear; a second click within the window confirms; a second click
 *      after the window lapses re-arms instead of confirming.
 *   4. (folded into 3, see test4) stale arm re-arming, then confirming.
 *   5. routing — the one genuinely new behavior in this build step: closes
 *      A via a routing change (not silence-timeout or buffer-full), and A's
 *      decay keeps progressing on its own after B becomes the active target.
 *   6. memory-word buckets (Build/Test Plan step 4) — every "NN% (word)"
 *      reading loopA_status produces while Looping matches the design doc's
 *      Vivid/Fading/Hazy/Almost gone boundaries, and all four are observed.
 *   7. master_loops_overview format (step 4) — the Master page's knob-6
 *      readout: all-idle, one loop Recording, and a fresh-close memory
 *      decile alongside a second loop Recording, each checked byte-exact.
 *   8. silence-timeout close — the third close condition, unexercised by
 *      1-7 (which use buffer-full or routing-change).
 *   9. too-short blip discard — close_recording's MIN_RECORDED_FRAMES floor.
 *  10. routing -> None closes a recording, same as routing to another letter.
 *  11. parameter clamping/bounds on every knob, plus a rejected (not
 *      clamped) out-of-range input_routing value.
 *  12. extreme decay_rate = 60 (the slow boundary; 1-7 only cover 3 and 20).
 *  13. a silence gap mid-recording shorter than silence_timeout does not
 *      false-close, and the timeout resets rather than just pausing.
 *  14. erase fired while Idle (no-op safety) and while Recording (hard
 *      closes immediately, discarding the take) — 3/4 only cover Looping.
 *  15. `state` get/set round-trip: live knobs survive into a fresh instance
 *      bit-for-bit; recorded content/playback position deliberately do not.
 *  16. saturation-stage passthrough: bit-exact identity at saturation=0
 *      (nonzero degrade) and at degrade=0 (any saturation) — the bug fixed
 *      in this same commit.
 *
 * Recordings closed deliberately via the buffer-full path (feed a full
 * buffer_seconds of continuous tone) rather than the silence-timeout path —
 * silence-timeout recordings bake in ~silence_timeout worth of near-silent
 * tail before closing (RECORDING keeps writing through the trailing pause),
 * which makes the resulting recorded_length depend on envelope-decay timing.
 * Buffer-full gives an exact, known recorded_length with no such fuzziness.
 * Test 5 is the exception: it deliberately does NOT let A reach buffer-full
 * or silence-timeout, since proving the routing-change close fired requires
 * that neither of the other two closes could have.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v2.h"

extern audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host);

#define TEST_PI_F 3.14159265358979323846f

#define SAMPLE_RATE   44100
#define BLOCK_FRAMES  128

/* Mirrors forgetful.c's documented defaults (design doc: 50ms debounce,
 * 8s buffer, 600ms erase confirm, 400ms forgotten display). */
#define TEST_DEBOUNCE_FRAMES          (50L  * SAMPLE_RATE / 1000)
#define TEST_BUFFER_CAPACITY_FRAMES   (44100L * 8)
#define TEST_ERASE_CONFIRM_FRAMES     (600L * SAMPLE_RATE / 1000)
#define TEST_FORGOTTEN_DISPLAY_FRAMES (400L * SAMPLE_RATE / 1000)
#define TEST_SILENCE_TIMEOUT_FRAMES   (1500L * SAMPLE_RATE / 1000)
#define TEST_MIN_RECORDED_FRAMES      (50L   * SAMPLE_RATE / 1000)

/* Mirrors forgetful.c's ROUTE_* — matches the declared options index order
 * ["None","A","B","C","D"] that input_routing's set_param parses. */
#define TEST_ROUTE_NONE "0"
#define TEST_ROUTE_A    "1"
#define TEST_ROUTE_B    "2"

static int g_failures = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        g_failures++;
    }
}

static void fill_tone(int16_t *buf, int frames, float amplitude, float freq, float *phase) {
    for (int i = 0; i < frames; i++) {
        float samp = amplitude * sinf(*phase);
        int16_t v = (int16_t)(samp * 32767.0f);
        buf[i * 2]     = v;
        buf[i * 2 + 1] = v;
        *phase += 2.0f * TEST_PI_F * freq / SAMPLE_RATE;
        if (*phase >= 2.0f * TEST_PI_F) *phase -= 2.0f * TEST_PI_F;
    }
}

static void fill_silence(int16_t *buf, int frames) {
    memset(buf, 0, sizeof(int16_t) * 2 * (size_t)frames);
}

/* A constant (DC-ish) "tone" — used only by the saturation-passthrough test
 * (test16), where a value that never changes sample-to-sample makes the
 * recorded content immune to the debounce-triggered start-offset jitter that
 * a real sine tone would carry: any window of a constant signal is identical
 * regardless of exactly which sample write_head==0 landed on. */
static void fill_constant(int16_t *buf, int frames, int16_t value) {
    for (int i = 0; i < frames; i++) {
        buf[i * 2]     = value;
        buf[i * 2 + 1] = value;
    }
}

static const char *status_of(audio_fx_api_v2_t *api, void *inst, char letter) {
    static char key[32];
    static char buf[64];
    snprintf(key, sizeof(key), "loop%c_status", letter);
    int n = api->get_param(inst, key, buf, sizeof(buf));
    if (n <= 0) buf[0] = '\0';
    return buf;
}

/* loopX_status is now a full state line (Build/Test Plan step 4), not a bare
 * state name — "Idle" became "Listening..." and "Looping" became
 * "Looping - NN% (word)" with a live percentage, so the LOOPING checks below
 * match the fixed prefix rather than the whole string. */
static int status_is_looping(const char *status) {
    return strncmp(status, "Looping - ", 10) == 0;
}
#define STATUS_LISTENING "Listening..."

static const char *erase_readout(audio_fx_api_v2_t *api, void *inst, char letter) {
    static char key[32];
    static char buf[64];
    snprintf(key, sizeof(key), "loop%c_erase", letter);
    int n = api->get_param(inst, key, buf, sizeof(buf));
    if (n <= 0) buf[0] = '\0';
    return buf;
}

static void run_tone(audio_fx_api_v2_t *api, void *inst, long total_frames,
                      float amplitude, float freq, float *phase) {
    int16_t buf[BLOCK_FRAMES * 2];
    long remaining = total_frames;
    while (remaining > 0) {
        int n = remaining < BLOCK_FRAMES ? (int)remaining : BLOCK_FRAMES;
        fill_tone(buf, n, amplitude, freq, phase);
        api->process_block(inst, buf, n);
        remaining -= n;
    }
}

static void run_silence(audio_fx_api_v2_t *api, void *inst, long total_frames) {
    int16_t buf[BLOCK_FRAMES * 2];
    long remaining = total_frames;
    while (remaining > 0) {
        int n = remaining < BLOCK_FRAMES ? (int)remaining : BLOCK_FRAMES;
        fill_silence(buf, n);
        api->process_block(inst, buf, n);
        remaining -= n;
    }
}

static void run_constant(audio_fx_api_v2_t *api, void *inst, long total_frames, int16_t value) {
    int16_t buf[BLOCK_FRAMES * 2];
    long remaining = total_frames;
    while (remaining > 0) {
        int n = remaining < BLOCK_FRAMES ? (int)remaining : BLOCK_FRAMES;
        fill_constant(buf, n, value);
        api->process_block(inst, buf, n);
        remaining -= n;
    }
}

/* Records a full buffer of tone into loop A (closes via buffer-full, not
 * silence-timeout) so recorded_length is exactly TEST_BUFFER_CAPACITY_FRAMES,
 * deterministically. total_frames only advances in whole BLOCK_FRAMES steps,
 * so the debounce can complete a little after the nominal TEST_DEBOUNCE_FRAMES
 * mark (rounded up to the next block boundary) — the extra slack guarantees
 * the buffer actually fills rather than landing a few samples short. */
static void record_full_buffer_loop_a(audio_fx_api_v2_t *api, void *inst, float *phase) {
    api->set_param(inst, "input_routing", TEST_ROUTE_A);
    run_tone(api, inst, TEST_DEBOUNCE_FRAMES + TEST_BUFFER_CAPACITY_FRAMES + BLOCK_FRAMES * 8,
             0.5f, 440.0f, phase);
}

/* Constant-value counterpart of record_full_buffer_loop_a, for test16: fills
 * the whole buffer with one unchanging sample value, so the recorded content
 * is known exactly (`value`, every index) without needing to reconstruct the
 * debounce-triggered start offset. */
static void record_full_buffer_loop_a_constant(audio_fx_api_v2_t *api, void *inst, int16_t value) {
    api->set_param(inst, "input_routing", TEST_ROUTE_A);
    run_constant(api, inst, TEST_DEBOUNCE_FRAMES + TEST_BUFFER_CAPACITY_FRAMES + BLOCK_FRAMES * 8, value);
}

int main(void) {
    host_api_v1_t host;
    memset(&host, 0, sizeof(host));
    host.api_version = MOVE_PLUGIN_API_VERSION;

    audio_fx_api_v2_t *api = move_audio_fx_init_v2(&host);
    if (!api) { fprintf(stderr, "FAIL: move_audio_fx_init_v2 returned NULL\n"); return 1; }
    if (!api->create_instance || !api->destroy_instance || !api->process_block ||
        !api->set_param || !api->get_param) {
        fprintf(stderr, "FAIL: API missing required callbacks\n");
        return 1;
    }

    /* ---- Test 0: chain_params / ui_hierarchy shape ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test0: create_instance");

        char cp[8192], hier[2048], probe[64];

        int n = api->get_param(inst, "chain_params", cp, sizeof(cp));
        check(n > 0, "test0: chain_params readable");
        check(n > 0 && cp[0] == '[', "test0: chain_params is an array");

        int key_count = 0;
        const char *p = cp;
        while ((p = strstr(p, "\"key\":\"")) != NULL) { key_count++; p += 7; }
        check(key_count == 40, "test0: chain_params has exactly 40 entries (5 pages x 8)");

        n = api->get_param(inst, "ui_hierarchy", hier, sizeof(hier));
        check(n > 0, "test0: ui_hierarchy readable");
        check(n > 0 && strstr(hier, "\"knobs\":[") != NULL, "test0: ui_hierarchy has a knobs array");

        /* every expected key, in both blobs */
        static const char *master_keys[] = {
            "input_routing", "loopA_volume", "loopB_volume", "loopC_volume", "loopD_volume",
            "master_loops_overview", "master_reserved_1", "master_reserved_2"
        };
        for (size_t i = 0; i < sizeof(master_keys) / sizeof(master_keys[0]); i++) {
            snprintf(probe, sizeof(probe), "\"key\":\"%s\"", master_keys[i]);
            check(strstr(cp, probe) != NULL, "test0: chain_params contains master key");
            snprintf(probe, sizeof(probe), "\"%s\"", master_keys[i]);
            check(strstr(hier, probe) != NULL, "test0: ui_hierarchy contains master key");
        }
        static const char *loop_suffixes[] = {
            "decay_rate", "wow", "hf_loss", "hiss", "saturation", "chaos", "reserved", "erase"
        };
        static const char letters[] = { 'A', 'B', 'C', 'D' };
        for (int li = 0; li < 4; li++) {
            for (size_t si = 0; si < sizeof(loop_suffixes) / sizeof(loop_suffixes[0]); si++) {
                snprintf(probe, sizeof(probe), "\"key\":\"loop%c_%s\"", letters[li], loop_suffixes[si]);
                check(strstr(cp, probe) != NULL, "test0: chain_params contains per-loop key");
                snprintf(probe, sizeof(probe), "\"loop%c_%s\"", letters[li], loop_suffixes[si]);
                check(strstr(hier, probe) != NULL, "test0: ui_hierarchy contains per-loop key");
            }
        }

        api->destroy_instance(inst);
    }

    /* ---- Test 1: record trigger — debounce timing + dry passthrough (Loop A) ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test1: create_instance");
        api->set_param(inst, "input_routing", TEST_ROUTE_A);

        float phase = 0.0f;
        int16_t buf[BLOCK_FRAMES * 2], ref[BLOCK_FRAMES * 2];

        /* Below record_threshold (-30dBFS ~= 0.0316 linear): must stay Idle,
         * and dry must pass through untouched the whole time. */
        for (int b = 0; b < 20; b++) {
            fill_tone(buf, BLOCK_FRAMES, 0.005f, 440.0f, &phase);
            memcpy(ref, buf, sizeof(buf));
            api->process_block(inst, buf, BLOCK_FRAMES);
            check(memcmp(buf, ref, sizeof(buf)) == 0, "test1: dry passthrough below threshold");
        }
        check(strcmp(status_of(api, inst, 'A'), STATUS_LISTENING) == 0, "test1: stays Idle below threshold");

        /* Above threshold, but short of the debounce window: still Idle,
         * dry still passes through (this is the state the original plan's
         * open question #1 was about — resolved as "always dry"). */
        long fed = 0;
        int saw_recording_early = 0;
        long pre_debounce_cutoff = TEST_DEBOUNCE_FRAMES - BLOCK_FRAMES * 3;
        while (fed < pre_debounce_cutoff) {
            fill_tone(buf, BLOCK_FRAMES, 0.5f, 440.0f, &phase);
            memcpy(ref, buf, sizeof(buf));
            api->process_block(inst, buf, BLOCK_FRAMES);
            check(memcmp(buf, ref, sizeof(buf)) == 0, "test1: dry passthrough during debounce");
            if (strcmp(status_of(api, inst, 'A'), "Recording") == 0) saw_recording_early = 1;
            fed += BLOCK_FRAMES;
        }
        check(!saw_recording_early, "test1: must not start Recording before debounce elapses");

        /* Push well past the debounce window: now Recording, dry still passes through. */
        long post_debounce_target = TEST_DEBOUNCE_FRAMES + BLOCK_FRAMES * 6;
        while (fed < post_debounce_target) {
            fill_tone(buf, BLOCK_FRAMES, 0.5f, 440.0f, &phase);
            memcpy(ref, buf, sizeof(buf));
            api->process_block(inst, buf, BLOCK_FRAMES);
            check(memcmp(buf, ref, sizeof(buf)) == 0, "test1: dry passthrough while Recording");
            fed += BLOCK_FRAMES;
        }
        check(strcmp(status_of(api, inst, 'A'), "Recording") == 0, "test1: Recording after debounce elapses");

        api->destroy_instance(inst);
    }

    /* ---- Test 2: decay timing + forgotten_at display window (Loop A) ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test2: create_instance");

        api->set_param(inst, "loopA_decay_rate", "3"); /* minimum: forgotten after 3 wraps */

        float phase = 0.0f;
        record_full_buffer_loop_a(api, inst, &phase);
        check(status_is_looping(status_of(api, inst, 'A')), "test2: Looping after buffer-full close");

        /* ~3 wraps of a TEST_BUFFER_CAPACITY_FRAMES-length loop, with slack
         * for wow/flutter's small speed modulation. */
        long budget = TEST_BUFFER_CAPACITY_FRAMES * 3 + 50000;
        long advanced = 0;
        int saw_forgotten = 0;
        int16_t buf[BLOCK_FRAMES * 2];
        while (advanced < budget && !saw_forgotten) {
            fill_silence(buf, BLOCK_FRAMES);
            api->process_block(inst, buf, BLOCK_FRAMES);
            if (strcmp(status_of(api, inst, 'A'), "Forgotten") == 0) saw_forgotten = 1;
            advanced += BLOCK_FRAMES;
        }
        check(saw_forgotten, "test2: status reports Forgotten within the expected decay budget");

        /* Run past the display window: status settles to Idle. */
        run_silence(api, inst, TEST_FORGOTTEN_DISPLAY_FRAMES + BLOCK_FRAMES * 4);
        check(strcmp(status_of(api, inst, 'A'), STATUS_LISTENING) == 0, "test2: status settles to Idle after the display window");

        /* Engine is genuinely reset — a fresh tone can start recording again
         * immediately (still routed to A from record_full_buffer_loop_a). */
        run_tone(api, inst, TEST_DEBOUNCE_FRAMES + BLOCK_FRAMES, 0.5f, 440.0f, &phase);
        check(strcmp(status_of(api, inst, 'A'), "Recording") == 0,
              "test2: engine accepts a new recording right after Forgotten");

        api->destroy_instance(inst);
    }

    /* ---- Test 3: double-click erase within the confirm window (Loop A) ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test3: create_instance");

        float phase = 0.0f;
        record_full_buffer_loop_a(api, inst, &phase);
        check(status_is_looping(status_of(api, inst, 'A')), "test3: Looping before erase");

        api->set_param(inst, "loopA_erase", "Erase!");
        check(strcmp(erase_readout(api, inst, 'A'), "Tap again") == 0, "test3: single click arms");
        check(status_is_looping(status_of(api, inst, 'A')), "test3: single click does not clear");

        run_silence(api, inst, BLOCK_FRAMES * 4); /* well under the confirm window */
        api->set_param(inst, "loopA_erase", "Erase!");
        check(strcmp(erase_readout(api, inst, 'A'), "-") == 0, "test3: confirmed erase clears the arm");
        check(strcmp(status_of(api, inst, 'A'), STATUS_LISTENING) == 0, "test3: confirmed erase drops to Idle");

        api->destroy_instance(inst);
    }

    /* ---- Test 4: stale arm past the confirm window does not confirm (Loop A) ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test4: create_instance");

        float phase = 0.0f;
        record_full_buffer_loop_a(api, inst, &phase);
        check(status_is_looping(status_of(api, inst, 'A')), "test4: Looping before erase");

        api->set_param(inst, "loopA_erase", "Erase!");
        check(strcmp(erase_readout(api, inst, 'A'), "Tap again") == 0, "test4: first click arms");

        run_silence(api, inst, TEST_ERASE_CONFIRM_FRAMES + BLOCK_FRAMES * 4); /* let the window lapse */

        api->set_param(inst, "loopA_erase", "Erase!");
        check(strcmp(erase_readout(api, inst, 'A'), "Tap again") == 0,
              "test4: stale click re-arms rather than confirming");
        check(status_is_looping(status_of(api, inst, 'A')), "test4: stale click must not clear the loop");

        run_silence(api, inst, BLOCK_FRAMES * 4); /* well under the fresh window */
        api->set_param(inst, "loopA_erase", "Erase!");
        check(strcmp(erase_readout(api, inst, 'A'), "-") == 0, "test4: fresh arm confirms normally");
        check(strcmp(status_of(api, inst, 'A'), STATUS_LISTENING) == 0, "test4: fresh-arm confirm drops to Idle");

        api->destroy_instance(inst);
    }

    /* ---- Test 5: routing — the new behavior in this build step.
     * Closes A via a routing change (not silence-timeout or buffer-full),
     * and A's decay keeps progressing on its own once B is the active
     * recording target. ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test5: create_instance");

        api->set_param(inst, "loopA_decay_rate", "3"); /* fast decay: small test budget */

        /* Route to A and record a short, deliberately UNCLOSED take — no
         * silence fed, nowhere near buffer-full, so the close that follows
         * can only have come from the routing change itself. */
        api->set_param(inst, "input_routing", TEST_ROUTE_A);
        float phase_a = 0.0f;
        long a_take_frames = TEST_DEBOUNCE_FRAMES + 20000;
        run_tone(api, inst, a_take_frames, 0.5f, 440.0f, &phase_a);
        check(strcmp(status_of(api, inst, 'A'), "Recording") == 0,
              "test5: A is Recording before the routing change");

        /* Switch routing away from A mid-recording. This must close A
         * SYNCHRONOUSLY, inside this very set_param call. */
        api->set_param(inst, "input_routing", TEST_ROUTE_B);
        check(status_is_looping(status_of(api, inst, 'A')),
              "test5: A closes into Looping the instant routing moves away "
              "(proves the close came from the routing change, not silence-timeout)");

        /* Record a DIFFERENT tone into B, for long enough that A's short
         * loop (decay_rate=3) has time to fully decay on its own,
         * unattended — A never receives input again after this point. */
        float phase_b = 0.0f;
        long budget = a_take_frames * 3 + 50000; /* ~3 wraps of A's take, generous slack */
        long advanced = 0;
        int saw_b_recording = 0, saw_a_forgotten = 0;
        int16_t buf[BLOCK_FRAMES * 2];
        while (advanced < budget) {
            fill_tone(buf, BLOCK_FRAMES, 0.5f, 880.0f, &phase_b);
            api->process_block(inst, buf, BLOCK_FRAMES);
            if (strcmp(status_of(api, inst, 'B'), "Recording") == 0) saw_b_recording = 1;
            if (strcmp(status_of(api, inst, 'A'), "Forgotten") == 0) saw_a_forgotten = 1;
            advanced += BLOCK_FRAMES;
        }
        check(saw_b_recording, "test5: B records the different tone while A is no longer routed");
        check(saw_a_forgotten, "test5: A's memory keeps decaying on its own while B is the active target");

        /* B must be unaffected by A's independent decay/reset. */
        check(strcmp(status_of(api, inst, 'B'), "Recording") == 0,
              "test5: B is unaffected by A's independent decay and reset");

        api->destroy_instance(inst);
    }

    /* ---- Test 6: memory-word bucket mapping in the loop-page status line
     * (Loop A) - Build/Test Plan step 4 ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test6: create_instance");

        api->set_param(inst, "loopA_decay_rate", "20"); /* 5%-per-wrap steps */

        float phase = 0.0f;
        record_full_buffer_loop_a(api, inst, &phase);
        check(status_is_looping(status_of(api, inst, 'A')), "test6: Looping after buffer-full close");

        /* Poll every block through ~20 wraps plus slack for wow/flutter's
         * small speed modulation, and check every observed "NN% (word)"
         * reading against the design doc's bucket boundaries directly -
         * this doesn't depend on landing on any particular wrap, so it's
         * robust to the read-speed jitter that makes exact wrap counting
         * impractical (see test2's comment on the same issue). */
        long budget = TEST_BUFFER_CAPACITY_FRAMES * 20 + 500000;
        long advanced = 0;
        int saw_vivid = 0, saw_fading = 0, saw_hazy = 0, saw_almost_gone = 0;
        int bucket_mismatch = 0;
        int16_t buf[BLOCK_FRAMES * 2];
        while (advanced < budget && strcmp(status_of(api, inst, 'A'), "Forgotten") != 0) {
            fill_silence(buf, BLOCK_FRAMES);
            api->process_block(inst, buf, BLOCK_FRAMES);
            const char *s = status_of(api, inst, 'A');
            int pct;
            char word[32];
            if (sscanf(s, "Looping - %d%% (%31[^)])", &pct, word) == 2) {
                const char *expected =
                    pct >= 90 ? "Vivid" :
                    pct >= 40 ? "Fading" :
                    pct >= 10 ? "Hazy" : "Almost gone";
                if (strcmp(word, expected) != 0) bucket_mismatch = 1;
                if (strcmp(word, "Vivid") == 0)       saw_vivid = 1;
                if (strcmp(word, "Fading") == 0)      saw_fading = 1;
                if (strcmp(word, "Hazy") == 0)        saw_hazy = 1;
                if (strcmp(word, "Almost gone") == 0) saw_almost_gone = 1;
            }
            advanced += BLOCK_FRAMES;
        }
        check(!bucket_mismatch, "test6: every observed percentage matches the design doc's word bucket");
        check(saw_vivid,       "test6: observed the Vivid bucket (90-100%)");
        check(saw_fading,      "test6: observed the Fading bucket (40-89%)");
        check(saw_hazy,        "test6: observed the Hazy bucket (10-39%)");
        check(saw_almost_gone, "test6: observed the Almost gone bucket (1-9%)");

        api->destroy_instance(inst);
    }

    /* ---- Test 7: master_loops_overview format (Master page knob 6) -
     * Build/Test Plan step 4 ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test7: create_instance");

        char buf[16];
        int n = api->get_param(inst, "master_loops_overview", buf, sizeof(buf));
        check(n == 4, "test7: overview is exactly 4 characters");
        check(strcmp(buf, "----") == 0, "test7: fresh instance reads all-idle");

        /* Route to A, well past debounce, and (unlike test1/test5's shorter
         * "just confirm Recording" probes) far enough past it that write_head
         * clears the too-short-a-take discard floor too - this take gets
         * CLOSED below via a routing change, and a discarded take would read
         * back Idle instead of Looping. */
        api->set_param(inst, "input_routing", TEST_ROUTE_A);
        float phase = 0.0f;
        long a_take_frames = TEST_DEBOUNCE_FRAMES + 20000;
        run_tone(api, inst, a_take_frames, 0.5f, 440.0f, &phase);
        check(strcmp(status_of(api, inst, 'A'), "Recording") == 0, "test7: A is Recording");
        n = api->get_param(inst, "master_loops_overview", buf, sizeof(buf));
        check(n == 4 && strcmp(buf, "R---") == 0, "test7: overview shows A recording, rest idle");

        /* Switch routing to B: closes A into Looping at memory=1.0 - the top
         * decile, digit '9' (the decile scheme has no distinct digit for
         * "100%" versus "90-99%", by design - see the header comment). */
        api->set_param(inst, "input_routing", TEST_ROUTE_B);
        n = api->get_param(inst, "master_loops_overview", buf, sizeof(buf));
        check(n == 4 && strcmp(buf, "9---") == 0, "test7: overview shows A's fresh-close decile, rest idle");

        /* B now records too: overview reflects both loops at once. */
        float phase_b = 0.0f;
        run_tone(api, inst, TEST_DEBOUNCE_FRAMES + BLOCK_FRAMES * 6, 0.5f, 880.0f, &phase_b);
        check(strcmp(status_of(api, inst, 'B'), "Recording") == 0, "test7: B is Recording");
        n = api->get_param(inst, "master_loops_overview", buf, sizeof(buf));
        check(n == 4 && strcmp(buf, "9R--") == 0, "test7: overview reflects both A and B simultaneously");

        api->destroy_instance(inst);
    }

    /* ---- Test 8: silence-timeout close — the one close condition (of the
     * three the design doc names: silence-timeout, buffer-full, routing-
     * change) that tests 1-7 never actually exercise; every earlier test
     * closes via buffer-full or a routing change. ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test8: create_instance");
        api->set_param(inst, "input_routing", TEST_ROUTE_A);

        float phase = 0.0f;
        run_tone(api, inst, TEST_DEBOUNCE_FRAMES + 20000, 0.5f, 440.0f, &phase);
        check(strcmp(status_of(api, inst, 'A'), "Recording") == 0, "test8: Recording before silence");

        run_silence(api, inst, TEST_SILENCE_TIMEOUT_FRAMES + BLOCK_FRAMES * 4);
        check(status_is_looping(status_of(api, inst, 'A')),
              "test8: silence-timeout closes the recording into Looping on its own "
              "(recorded content is far short of buffer_seconds, so this can only "
              "be the silence-timeout close)");

        api->destroy_instance(inst);
    }

    /* ---- Test 9: too-short blip discard — close_recording's
     * MIN_RECORDED_FRAMES floor ("discard a too-short take rather than loop
     * a click") has no coverage anywhere else: every other test's take is
     * either buffer-full (deliberately long) or several thousand frames past
     * debounce. Feed only ~one block of tone past the debounce trigger, then
     * close via a ROUTING CHANGE rather than silence-timeout: RECORDING
     * keeps writing every sample regardless of input level right up until
     * the close actually fires, so a silence-timeout close would let
     * write_head balloon past MIN_RECORDED_FRAMES during the ~1.5s timeout
     * wait (confirmed by running it that way first — the take was no longer
     * short by the time it closed). Routing-change is synchronous and
     * instant, the only close that can catch write_head while it's still
     * genuinely tiny. The loop must discard back to Idle, not start Looping
     * a click. ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test9: create_instance");
        api->set_param(inst, "input_routing", TEST_ROUTE_A);

        float phase = 0.0f;
        /* One block past the point debounce completes: write_head accumulates
         * roughly a block's worth of frames, comfortably under
         * TEST_MIN_RECORDED_FRAMES (both 50ms in forgetful.c, same as the
         * debounce window itself). */
        run_tone(api, inst, TEST_DEBOUNCE_FRAMES + BLOCK_FRAMES, 0.5f, 440.0f, &phase);
        check(strcmp(status_of(api, inst, 'A'), "Recording") == 0, "test9: blip does start Recording");

        api->set_param(inst, "input_routing", TEST_ROUTE_NONE);
        check(strcmp(status_of(api, inst, 'A'), STATUS_LISTENING) == 0,
              "test9: a too-short take is discarded back to Idle, not looped");

        api->destroy_instance(inst);
    }

    /* ---- Test 10: routing -> None closes the active recording too — test5
     * only exercises routing moving to a DIFFERENT letter (A->B); moving to
     * None is a distinct code path (routed_index becomes -1) that the design
     * doc names as the same kind of close ("either to a different letter or
     * to None"). ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test10: create_instance");
        api->set_param(inst, "input_routing", TEST_ROUTE_A);

        float phase = 0.0f;
        run_tone(api, inst, TEST_DEBOUNCE_FRAMES + 20000, 0.5f, 440.0f, &phase);
        check(strcmp(status_of(api, inst, 'A'), "Recording") == 0, "test10: A is Recording before routing to None");

        api->set_param(inst, "input_routing", TEST_ROUTE_NONE);
        check(status_is_looping(status_of(api, inst, 'A')),
              "test10: routing to None closes A synchronously, same as routing to another letter");

        /* With nothing routed, no other loop should start recording even
         * when fed tone. */
        float phase_bcd = 0.0f;
        run_tone(api, inst, TEST_DEBOUNCE_FRAMES + BLOCK_FRAMES * 6, 0.5f, 660.0f, &phase_bcd);
        check(strcmp(status_of(api, inst, 'B'), STATUS_LISTENING) == 0,
              "test10: with routing None, B does not start recording even with input present");
        check(strcmp(status_of(api, inst, 'C'), STATUS_LISTENING) == 0,
              "test10: with routing None, C does not start recording even with input present");
        check(strcmp(status_of(api, inst, 'D'), STATUS_LISTENING) == 0,
              "test10: with routing None, D does not start recording even with input present");

        api->destroy_instance(inst);
    }

    /* ---- Test 11: parameter clamping — an out-of-range set_param value
     * clamps to the declared range instead of storing the raw value, and an
     * out-of-range input_routing value is REJECTED outright (leaves the
     * existing route untouched) rather than clamped, since routing has
     * discrete valid states with no sensible "nearest" clamp. No existing
     * test sends an out-of-range value to any knob. ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test11: create_instance");
        char buf[64];

        api->set_param(inst, "loopA_decay_rate", "1000");
        api->get_param(inst, "loopA_decay_rate", buf, sizeof(buf));
        check(strcmp(buf, "60.0") == 0, "test11: decay_rate clamps to max 60");
        api->set_param(inst, "loopA_decay_rate", "-5");
        api->get_param(inst, "loopA_decay_rate", buf, sizeof(buf));
        check(strcmp(buf, "3.0") == 0, "test11: decay_rate clamps to min 3");

        static const char *unit_keys[] = { "wow", "hf_loss", "hiss", "saturation", "chaos" };
        for (size_t i = 0; i < sizeof(unit_keys) / sizeof(unit_keys[0]); i++) {
            char key[32];
            snprintf(key, sizeof(key), "loopA_%s", unit_keys[i]);
            api->set_param(inst, key, "5");
            api->get_param(inst, key, buf, sizeof(buf));
            check(strcmp(buf, "1.000") == 0, "test11: 0..1 flavor param clamps to max 1");
            api->set_param(inst, key, "-5");
            api->get_param(inst, key, buf, sizeof(buf));
            check(strcmp(buf, "0.000") == 0, "test11: 0..1 flavor param clamps to min 0");
        }

        api->set_param(inst, "loopA_volume", "5");
        api->get_param(inst, "loopA_volume", buf, sizeof(buf));
        check(strcmp(buf, "1.000") == 0, "test11: loop volume clamps to max 1");
        api->set_param(inst, "loopA_volume", "-5");
        api->get_param(inst, "loopA_volume", buf, sizeof(buf));
        check(strcmp(buf, "0.000") == 0, "test11: loop volume clamps to min 0");

        api->set_param(inst, "input_routing", TEST_ROUTE_A);
        api->get_param(inst, "input_routing", buf, sizeof(buf));
        check(strcmp(buf, "A") == 0, "test11: valid input_routing accepted");
        api->set_param(inst, "input_routing", "99");
        api->get_param(inst, "input_routing", buf, sizeof(buf));
        check(strcmp(buf, "A") == 0, "test11: out-of-range input_routing (too high) is rejected, route unchanged");
        api->set_param(inst, "input_routing", "-1");
        api->get_param(inst, "input_routing", buf, sizeof(buf));
        check(strcmp(buf, "A") == 0, "test11: out-of-range input_routing (negative) is rejected, route unchanged");

        api->destroy_instance(inst);
    }

    /* ---- Test 12: extreme decay_rate = 60 (the slow end; test2/test5/test6
     * all use the fast end (3) or the default (20) — the slow boundary has
     * no coverage). A handful of wraps at the slowest setting must still
     * report Looping with a believable, still-high percentage, not flip to
     * Forgotten early or drift outside the expected range. ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test12: create_instance");
        api->set_param(inst, "loopA_decay_rate", "60");

        float phase = 0.0f;
        record_full_buffer_loop_a(api, inst, &phase);
        check(status_is_looping(status_of(api, inst, 'A')), "test12: Looping after buffer-full close");

        /* 5 wraps at decay_rate=60 -> expected memory ~= 1 - 5/60 = 91.7%. */
        long budget = TEST_BUFFER_CAPACITY_FRAMES * 5 + 50000;
        long advanced = 0;
        int16_t buf[BLOCK_FRAMES * 2];
        while (advanced < budget) {
            fill_silence(buf, BLOCK_FRAMES);
            api->process_block(inst, buf, BLOCK_FRAMES);
            check(strcmp(status_of(api, inst, 'A'), "Forgotten") != 0,
                  "test12: must not reach Forgotten within 5 wraps at the slowest decay_rate");
            advanced += BLOCK_FRAMES;
        }
        const char *s = status_of(api, inst, 'A');
        check(status_is_looping(s), "test12: still Looping after 5 wraps at decay_rate=60");
        int pct = -1;
        char word[32];
        check(sscanf(s, "Looping - %d%% (%31[^)])", &pct, word) == 2, "test12: status line parses");
        check(pct >= 85 && pct <= 95,
              "test12: memory after 5 wraps at decay_rate=60 is in the expected ~92% neighborhood");

        api->destroy_instance(inst);
    }

    /* ---- Test 13: a silence gap mid-recording shorter than silence_timeout
     * must NOT close the recording — recording resumes and eventually closes
     * normally once a genuine full-length silence follows. Nothing in
     * tests 1-10 proves the timeout actually resets when input comes back;
     * they only prove the two extremes (never silent, or silent long enough
     * to close). ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test13: create_instance");
        api->set_param(inst, "input_routing", TEST_ROUTE_A);

        float phase = 0.0f;
        run_tone(api, inst, TEST_DEBOUNCE_FRAMES + BLOCK_FRAMES * 6, 0.5f, 440.0f, &phase);
        check(strcmp(status_of(api, inst, 'A'), "Recording") == 0, "test13: Recording before the gap");

        /* A gap well under the timeout — must not close. */
        run_silence(api, inst, TEST_SILENCE_TIMEOUT_FRAMES / 2);
        check(strcmp(status_of(api, inst, 'A'), "Recording") == 0,
              "test13: a silence gap shorter than silence_timeout does not close the recording");

        /* Input resumes: silence_since must reset, not just pause. */
        run_tone(api, inst, BLOCK_FRAMES * 10, 0.5f, 440.0f, &phase);
        check(strcmp(status_of(api, inst, 'A'), "Recording") == 0,
              "test13: still Recording after input resumes mid-gap");

        /* Now a genuine full timeout: closes normally. */
        run_silence(api, inst, TEST_SILENCE_TIMEOUT_FRAMES + BLOCK_FRAMES * 4);
        check(status_is_looping(status_of(api, inst, 'A')),
              "test13: a full silence_timeout after the resume closes the recording normally");

        api->destroy_instance(inst);
    }

    /* ---- Test 14: erase in the two states test3/test4 never exercise (both
     * only fire while Looping). The design doc says erase "works identically
     * regardless of Input Routing state"; it should also work identically
     * regardless of the loop's OWN state. ---- */
    {
        /* 14a: firing erase on a fresh, empty (Idle) loop must not crash or
         * misbehave — it arms and then confirms into... still Idle,
         * harmlessly. */
        void *inst_idle = api->create_instance(".", NULL);
        check(inst_idle != NULL, "test14a: create_instance");
        check(strcmp(status_of(api, inst_idle, 'A'), STATUS_LISTENING) == 0, "test14a: fresh loop is Idle");

        api->set_param(inst_idle, "loopA_erase", "Erase!");
        check(strcmp(erase_readout(api, inst_idle, 'A'), "Tap again") == 0, "test14a: erase arms even on an empty loop");
        api->set_param(inst_idle, "loopA_erase", "Erase!");
        check(strcmp(erase_readout(api, inst_idle, 'A'), "-") == 0, "test14a: confirm clears the arm");
        check(strcmp(status_of(api, inst_idle, 'A'), STATUS_LISTENING) == 0, "test14a: still Idle, no crash/misbehavior");

        api->destroy_instance(inst_idle);

        /* 14b: firing erase while actively Recording hard-closes it
         * immediately, discarding whatever was captured so far, rather than
         * needing to reach Looping first. */
        void *inst_rec = api->create_instance(".", NULL);
        check(inst_rec != NULL, "test14b: create_instance");
        api->set_param(inst_rec, "input_routing", TEST_ROUTE_A);

        float phase = 0.0f;
        run_tone(api, inst_rec, TEST_DEBOUNCE_FRAMES + 20000, 0.5f, 440.0f, &phase);
        check(strcmp(status_of(api, inst_rec, 'A'), "Recording") == 0, "test14b: Recording before erase");

        api->set_param(inst_rec, "loopA_erase", "Erase!");
        check(strcmp(erase_readout(api, inst_rec, 'A'), "Tap again") == 0, "test14b: first click arms while Recording");
        run_silence(api, inst_rec, BLOCK_FRAMES * 4);
        api->set_param(inst_rec, "loopA_erase", "Erase!");
        check(strcmp(status_of(api, inst_rec, 'A'), STATUS_LISTENING) == 0,
              "test14b: confirmed erase drops a Recording loop straight to Idle, discarding the take");

        /* A fresh recording can start immediately afterward. */
        run_tone(api, inst_rec, TEST_DEBOUNCE_FRAMES + BLOCK_FRAMES * 6, 0.5f, 440.0f, &phase);
        check(strcmp(status_of(api, inst_rec, 'A'), "Recording") == 0,
              "test14b: engine accepts a new recording right after an erase-during-Recording");

        api->destroy_instance(inst_rec);
    }

    /* ---- Test 15: `state` get/set round-trip — get_param("state") is used
     * for ordinary slot autosave/patch reload, and nothing exercises it at
     * all. Confirms: (a) live knobs (decay_rate + the five flavor params,
     * Master volumes, input_routing) survive a save/restore into a FRESH
     * instance bit-for-bit, and (b) recorded content/state is deliberately
     * NOT part of the blob — a Looping source loop must restore into an
     * Idle destination loop, not some half-restored Looping-with-no-buffer
     * state, and must not crash doing so. ---- */
    {
        void *src = api->create_instance(".", NULL);
        check(src != NULL, "test15: create_instance src");

        float phase = 0.0f;
        record_full_buffer_loop_a(api, src, &phase); /* gets A into Looping (randomizes flavor on close) */
        check(status_is_looping(status_of(api, src, 'A')), "test15: src loop A is Looping before save");

        /* Flavor knobs are live and override the randomized value
         * immediately (per the design doc) — set them to known, distinctive
         * values after the close so the saved blob is deterministic, not
         * whatever randomize_flavor() happened to draw. */
        api->set_param(src, "loopA_decay_rate", "45");
        api->set_param(src, "loopA_wow", "0.7");
        api->set_param(src, "loopA_hf_loss", "0.6");
        api->set_param(src, "loopA_hiss", "0.3");
        api->set_param(src, "loopA_saturation", "0.9");
        api->set_param(src, "loopA_chaos", "0.1");
        api->set_param(src, "loopB_volume", "0.25");
        api->set_param(src, "input_routing", TEST_ROUTE_B);

        char state_json[1024];
        int n = api->get_param(src, "state", state_json, sizeof(state_json));
        check(n > 0, "test15: state readable");

        void *dst = api->create_instance(".", NULL);
        check(dst != NULL, "test15: create_instance dst");
        api->set_param(dst, "state", state_json);

        char buf[64];
        api->get_param(dst, "loopA_decay_rate", buf, sizeof(buf));
        check(strcmp(buf, "45.0") == 0, "test15: decay_rate restored");
        api->get_param(dst, "loopA_wow", buf, sizeof(buf));
        check(strcmp(buf, "0.700") == 0, "test15: wow restored");
        api->get_param(dst, "loopA_hf_loss", buf, sizeof(buf));
        check(strcmp(buf, "0.600") == 0, "test15: hf_loss restored");
        api->get_param(dst, "loopA_hiss", buf, sizeof(buf));
        check(strcmp(buf, "0.300") == 0, "test15: hiss restored");
        api->get_param(dst, "loopA_saturation", buf, sizeof(buf));
        check(strcmp(buf, "0.900") == 0, "test15: saturation restored");
        api->get_param(dst, "loopA_chaos", buf, sizeof(buf));
        check(strcmp(buf, "0.100") == 0, "test15: chaos restored");
        api->get_param(dst, "loopB_volume", buf, sizeof(buf));
        check(strcmp(buf, "0.250") == 0, "test15: Master loop volume restored");
        api->get_param(dst, "input_routing", buf, sizeof(buf));
        check(strcmp(buf, "B") == 0, "test15: input_routing restored");

        /* The part that must NOT be restored: dst's loop A comes back Idle,
         * not Looping, even though src's loop A was Looping when the state
         * was saved — recorded buffer content and playback position are
         * deliberately outside the `state` blob. */
        check(strcmp(status_of(api, dst, 'A'), STATUS_LISTENING) == 0,
              "test15: dst loop A is Idle after restore, not carried over as Looping");

        api->destroy_instance(src);
        api->destroy_instance(dst);
    }

    /* ---- Test 16: saturation-stage passthrough — the fix in this commit.
     * Before the fix, sat_amount==0 still ran tanhf(x*1.0f)/tanhf(1.0f),
     * which is NOT identity — true both at the Warmth knob's literal minimum
     * AND for a freshly-closed loop (degrade==0) regardless of the Warmth
     * setting, since sat_amount = saturation * degrade. Uses a
     * constant-value recording (not a sine tone) so the recorded content is
     * known exactly without needing to reconstruct the debounce-triggered
     * start offset, and drives every other degrade-scaled stage
     * (wow/hf_loss/hiss/chaos) to zero — set AFTER the buffer-full close,
     * since randomize_flavor() overwrites them at RECORDING->LOOPING — so
     * the saturation stage's output is directly recoverable from
     * process_block's output samples (dry fed as silence during
     * measurement, so out == mix_dry_wet(0, wet) recovers wet to within
     * int16 rounding). ---- */
    {
        const int16_t CONST_VALUE = 16000; /* well above record_threshold, no clipping headroom issues */
        const float SAMPLE_F = (float)CONST_VALUE / 32768.0f;

        /* ---- 16a: saturation == 0, at NONZERO degrade (one wrap in) ---- */
        {
            void *inst = api->create_instance(".", NULL);
            check(inst != NULL, "test16a: create_instance");

            api->set_param(inst, "loopA_decay_rate", "3");
            record_full_buffer_loop_a_constant(api, inst, CONST_VALUE);
            check(status_is_looping(status_of(api, inst, 'A')), "test16a: Looping after buffer-full close");

            /* Set the isolation params AFTER the close (randomize_flavor
             * already ran). wow=0 makes read speed exactly 1.0, so feeding
             * exactly recorded_length (== TEST_BUFFER_CAPACITY_FRAMES)
             * frames of silence advances read_head through EXACTLY one
             * wrap, deterministically — memory afterward is exactly
             * 1.0f - 1.0f/decay_rate, matching forgetful.c's own expression
             * bit for bit. */
            api->set_param(inst, "loopA_wow", "0");
            api->set_param(inst, "loopA_hf_loss", "0");
            api->set_param(inst, "loopA_hiss", "0");
            api->set_param(inst, "loopA_chaos", "0");
            api->set_param(inst, "loopA_saturation", "0");
            api->set_param(inst, "loopA_volume", "1");

            run_silence(api, inst, TEST_BUFFER_CAPACITY_FRAMES);
            float expected_memory = 1.0f - 1.0f / 3.0f;
            check(expected_memory > 0.0f && expected_memory < 1.0f, "test16a: sanity — degrade is nonzero here");

            int16_t buf[BLOCK_FRAMES * 2];
            fill_silence(buf, BLOCK_FRAMES);
            api->process_block(inst, buf, BLOCK_FRAMES);

            /* Expected: filt_l == SAMPLE_F exactly (hf_loss=0, constant
             * content); sat_amount = saturation(0) * degrade(nonzero) == 0,
             * so under the fix sat_l == filt_l exactly regardless of
             * degrade. */
            float expected_wet = SAMPLE_F * expected_memory; /* * loop_volume(1) */
            int32_t expected_out = lroundf(expected_wet * 32767.0f);
            int mismatch = 0;
            for (int i = 0; i < BLOCK_FRAMES; i++) {
                if (abs((int)buf[i * 2] - (int)expected_out) > 1) mismatch = 1;
                if (abs((int)buf[i * 2 + 1] - (int)expected_out) > 1) mismatch = 1;
            }
            check(!mismatch,
                  "test16a: saturation=0 is bit-exact identity even at nonzero degrade "
                  "(within 1 LSB of independently-computed expected output)");

            api->destroy_instance(inst);
        }

        /* ---- 16b: saturation == 1 (max), at ZERO degrade (freshly closed) ---- */
        {
            void *inst = api->create_instance(".", NULL);
            check(inst != NULL, "test16b: create_instance");

            record_full_buffer_loop_a_constant(api, inst, CONST_VALUE);
            check(status_is_looping(status_of(api, inst, 'A')), "test16b: Looping after buffer-full close");

            /* Set post-close (randomize_flavor already ran) and measure the
             * very FIRST block — read_head has barely moved, nowhere near a
             * wrap, so memory is still exactly 1.0 and degrade is exactly
             * 0.0, regardless of the Warmth knob sitting at its maximum. */
            api->set_param(inst, "loopA_wow", "0");
            api->set_param(inst, "loopA_hf_loss", "0");
            api->set_param(inst, "loopA_hiss", "0");
            api->set_param(inst, "loopA_chaos", "0");
            api->set_param(inst, "loopA_saturation", "1");
            api->set_param(inst, "loopA_volume", "1");

            int16_t buf[BLOCK_FRAMES * 2];
            fill_silence(buf, BLOCK_FRAMES);
            api->process_block(inst, buf, BLOCK_FRAMES);

            float expected_wet = SAMPLE_F * 1.0f; /* memory == 1.0, degrade == 0.0 */
            int32_t expected_out = lroundf(expected_wet * 32767.0f);
            int mismatch = 0;
            for (int i = 0; i < BLOCK_FRAMES; i++) {
                if (abs((int)buf[i * 2] - (int)expected_out) > 1) mismatch = 1;
                if (abs((int)buf[i * 2 + 1] - (int)expected_out) > 1) mismatch = 1;
            }
            check(!mismatch,
                  "test16b: at degrade==0 (freshly closed) output is bit-exact identity "
                  "even with saturation at its maximum (within 1 LSB)");

            api->destroy_instance(inst);
        }
    }

    if (g_failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("PASS: forgetful LoopEngine bench test "
           "(chain_params shape, record trigger, decay timing, "
           "erase double-click/stale-rearm, routing, status-line word "
           "buckets, Loops Overview format, silence-timeout close, "
           "too-short blip discard, routing-to-None close, parameter "
           "clamping, extreme decay_rate, mid-recording silence gap, "
           "erase during Recording/Idle, state round-trip, saturation "
           "passthrough)\n");
    return 0;
}
