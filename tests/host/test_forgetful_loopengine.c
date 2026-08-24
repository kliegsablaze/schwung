/*
 * Bench test for Forgetful's Loop A LoopEngine (docs/plans/forgetful-design.md,
 * Build/Test Plan #5): synthetic tone/silence input driven straight through
 * the module's public v2 API (create_instance/set_param/get_param/
 * process_block), exactly as chain_host would drive it. Black-box — this
 * does not reach into forgetful.c's internals, so the timing constants below
 * mirror the design doc's documented defaults rather than forgetful.c's
 * private macros.
 *
 * Covers:
 *   1. record trigger — debounce timing, and dry input passing through
 *      unconditionally (IDLE and RECORDING both).
 *   2. decay timing — memory reaching 0 after decay_rate wraps, and the
 *      forgotten_at display window (get_param("loopA_status") reporting
 *      "Forgotten" for a while after the engine has already reset to Idle).
 *   3. double-click / stale-rearm erase — a single click arms but doesn't
 *      clear; a second click within the window confirms; a second click
 *      after the window lapses re-arms instead of confirming.
 *
 * Recordings in this test are closed via the buffer-full path (feed a full
 * buffer_seconds of continuous tone) rather than the silence-timeout path —
 * silence-timeout recordings bake in ~silence_timeout worth of near-silent
 * tail before closing (RECORDING keeps writing through the trailing pause),
 * which makes the resulting recorded_length depend on envelope-decay timing.
 * Buffer-full gives an exact, known recorded_length with no such fuzziness.
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

static const char *status_of(audio_fx_api_v2_t *api, void *inst) {
    static char buf[64];
    int n = api->get_param(inst, "loopA_status", buf, sizeof(buf));
    if (n <= 0) buf[0] = '\0';
    return buf;
}

static const char *erase_readout(audio_fx_api_v2_t *api, void *inst) {
    static char buf[64];
    int n = api->get_param(inst, "loopA_erase", buf, sizeof(buf));
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

/* Records a full buffer of tone (closes via buffer-full, not silence-timeout)
 * so recorded_length is exactly TEST_BUFFER_CAPACITY_FRAMES, deterministically.
 * total_frames only advances in whole BLOCK_FRAMES steps, so the debounce can
 * complete a little after the nominal TEST_DEBOUNCE_FRAMES mark (rounded up
 * to the next block boundary) — the extra slack guarantees the buffer
 * actually fills rather than landing a few samples short of capacity. */
static void record_full_buffer_loop(audio_fx_api_v2_t *api, void *inst, float *phase) {
    run_tone(api, inst, TEST_DEBOUNCE_FRAMES + TEST_BUFFER_CAPACITY_FRAMES + BLOCK_FRAMES * 8,
             0.5f, 440.0f, phase);
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

    /* ---- Test 1: record trigger — debounce timing + dry passthrough ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test1: create_instance");

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
        check(strcmp(status_of(api, inst), "Idle") == 0, "test1: stays Idle below threshold");

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
            if (strcmp(status_of(api, inst), "Recording") == 0) saw_recording_early = 1;
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
        check(strcmp(status_of(api, inst), "Recording") == 0, "test1: Recording after debounce elapses");

        api->destroy_instance(inst);
    }

    /* ---- Test 2: decay timing + forgotten_at display window ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test2: create_instance");

        api->set_param(inst, "loopA_decay_rate", "3"); /* minimum: forgotten after 3 wraps */

        float phase = 0.0f;
        record_full_buffer_loop(api, inst, &phase);
        check(strcmp(status_of(api, inst), "Looping") == 0, "test2: Looping after buffer-full close");

        /* ~3 wraps of a TEST_BUFFER_CAPACITY_FRAMES-length loop, with slack
         * for wow/flutter's small speed modulation. */
        long budget = TEST_BUFFER_CAPACITY_FRAMES * 3 + 50000;
        long advanced = 0;
        int saw_forgotten = 0;
        int16_t buf[BLOCK_FRAMES * 2];
        while (advanced < budget && !saw_forgotten) {
            fill_silence(buf, BLOCK_FRAMES);
            api->process_block(inst, buf, BLOCK_FRAMES);
            if (strcmp(status_of(api, inst), "Forgotten") == 0) saw_forgotten = 1;
            advanced += BLOCK_FRAMES;
        }
        check(saw_forgotten, "test2: status reports Forgotten within the expected decay budget");

        /* Run past the display window: status settles to Idle. */
        run_silence(api, inst, TEST_FORGOTTEN_DISPLAY_FRAMES + BLOCK_FRAMES * 4);
        check(strcmp(status_of(api, inst), "Idle") == 0, "test2: status settles to Idle after the display window");

        /* Engine is genuinely reset — a fresh tone can start recording again
         * immediately, even while still inside what would have been the old
         * display window's real-time span. */
        run_tone(api, inst, TEST_DEBOUNCE_FRAMES + BLOCK_FRAMES, 0.5f, 440.0f, &phase);
        check(strcmp(status_of(api, inst), "Recording") == 0,
              "test2: engine accepts a new recording right after Forgotten");

        api->destroy_instance(inst);
    }

    /* ---- Test 3: double-click erase within the confirm window ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test3: create_instance");

        float phase = 0.0f;
        record_full_buffer_loop(api, inst, &phase);
        check(strcmp(status_of(api, inst), "Looping") == 0, "test3: Looping before erase");

        api->set_param(inst, "loopA_erase", "Erase!");
        check(strcmp(erase_readout(api, inst), "Tap again") == 0, "test3: single click arms");
        check(strcmp(status_of(api, inst), "Looping") == 0, "test3: single click does not clear");

        run_silence(api, inst, BLOCK_FRAMES * 4); /* well under the confirm window */
        api->set_param(inst, "loopA_erase", "Erase!");
        check(strcmp(erase_readout(api, inst), "-") == 0, "test3: confirmed erase clears the arm");
        check(strcmp(status_of(api, inst), "Idle") == 0, "test3: confirmed erase drops to Idle");

        api->destroy_instance(inst);
    }

    /* ---- Test 4: stale arm past the confirm window does not confirm ---- */
    {
        void *inst = api->create_instance(".", NULL);
        check(inst != NULL, "test4: create_instance");

        float phase = 0.0f;
        record_full_buffer_loop(api, inst, &phase);
        check(strcmp(status_of(api, inst), "Looping") == 0, "test4: Looping before erase");

        api->set_param(inst, "loopA_erase", "Erase!");
        check(strcmp(erase_readout(api, inst), "Tap again") == 0, "test4: first click arms");

        run_silence(api, inst, TEST_ERASE_CONFIRM_FRAMES + BLOCK_FRAMES * 4); /* let the window lapse */

        api->set_param(inst, "loopA_erase", "Erase!");
        check(strcmp(erase_readout(api, inst), "Tap again") == 0,
              "test4: stale click re-arms rather than confirming");
        check(strcmp(status_of(api, inst), "Looping") == 0, "test4: stale click must not clear the loop");

        run_silence(api, inst, BLOCK_FRAMES * 4); /* well under the fresh window */
        api->set_param(inst, "loopA_erase", "Erase!");
        check(strcmp(erase_readout(api, inst), "-") == 0, "test4: fresh arm confirms normally");
        check(strcmp(status_of(api, inst), "Idle") == 0, "test4: fresh-arm confirm drops to Idle");

        api->destroy_instance(inst);
    }

    if (g_failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("PASS: forgetful LoopEngine bench test "
           "(record trigger, decay timing, erase double-click/stale-rearm)\n");
    return 0;
}
