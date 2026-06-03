/**
 * test_mcsigpack.c — unit tests and compression benchmarks
 *
 * Build:  make test
 * Run:    ./out/test_mcsigpack
 *
 * Regenerate biosignal test vectors (any duration):
 *   python3 tools/gen_dummy_data.py > test/dummy_data.h
 */

#include "mcsigpack.h"
#include "dummy_data.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Consistency checks between dummy_data.h and mcsigpack.h
 * ---------------------------------------------------------------------- */
_Static_assert(DUMMY_FS_HIGH == 250, "dummy data rate does not match EEG/ECG channel rate");
_Static_assert(DUMMY_FS_IMU  ==  20, "dummy data rate does not match IMU channel rate");
_Static_assert(DUMMY_N_HIGH  == DUMMY_FS_HIGH * DUMMY_DURATION, "DUMMY_N_HIGH inconsistent");
_Static_assert(DUMMY_N_IMU   == DUMMY_FS_IMU  * DUMMY_DURATION, "DUMMY_N_IMU inconsistent");

#define FULL_EPOCHS     (DUMMY_DURATION / MCSIGPACK_CHUNK_SECS)
#define EXPECTED_EPOCHS (FULL_EPOCHS + ((DUMMY_DURATION % MCSIGPACK_CHUNK_SECS) ? 1 : 0))

/* -------------------------------------------------------------------------
 * Test framework
 * ---------------------------------------------------------------------- */
static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  pass  %s\n", msg); g_pass++; } \
    else      { printf("  FAIL  %s  (line %d)\n", msg, __LINE__); g_fail++; } \
} while(0)

/* -------------------------------------------------------------------------
 * Packet capture — stands in for k_msgq_put on Zephyr
 * ---------------------------------------------------------------------- */
#define MAX_PKTS 8192

typedef struct {
    mcsigpack_packet_t pkts[MAX_PKTS];
    int                count;
    int                overflow;
} capture_t;

static void on_packet(const mcsigpack_packet_t *pkt, void *ud)
{
    capture_t *c = (capture_t *)ud;
    if (c->count < MAX_PKTS) c->pkts[c->count++] = *pkt;
    else                     c->overflow++;
}

static int bytes_for_channel(const capture_t *cap, uint8_t wire_id)
{
    int total = 0;
    for (int i = 0; i < cap->count; i++)
        if (cap->pkts[i].channel_id == wire_id)
            total += cap->pkts[i].payload_len;
    return total;
}

/* -------------------------------------------------------------------------
 * Shared helpers
 * ---------------------------------------------------------------------- */
static const int k_axes[MCSIGPACK_NUM_CHANNELS] = {
#define X(name, wire, hz, axes) (axes),
    MCSIGPACK_CHANNEL_LIST
#undef X
};

static const int k_hz[MCSIGPACK_NUM_CHANNELS] = {
#define X(name, wire, hz, axes) (hz),
    MCSIGPACK_CHANNEL_LIST
#undef X
};

/* Push one full synthetic epoch of a constant value to every channel,
 * interleaved at each channel's natural rate. */
static void fill_epoch(mcsigpack_ctx_t *ctx, int16_t value)
{
    int remaining[MCSIGPACK_NUM_CHANNELS];
    for (int c = 0; c < MCSIGPACK_NUM_CHANNELS; c++)
        remaining[c] = k_hz[c] * MCSIGPACK_CHUNK_SECS;

    int any = 1;
    while (any) {
        any = 0;
        for (int c = 0; c < MCSIGPACK_NUM_CHANNELS; c++) {
            if (remaining[c] <= 0) continue;
            mcsigpack_sample_t s;
            memset(&s, 0, sizeof(s));
            s.channel   = (mcsigpack_channel_e)c;
            s.values[0] = value;
            if (k_axes[c] >= 2) s.values[1] = value;
            if (k_axes[c] >= 3) s.values[2] = value;
            mcsigpack_push_sample(ctx, &s);
            remaining[c]--;
            any = 1;
        }
    }
}

/* Feed all dummy_data.h samples through in one pass, interleaved at natural
 * rates. Flushes at the end to emit any partial final epoch. */
static void feed_all_data(mcsigpack_ctx_t *ctx)
{
    const int HIGH_RATIO = DUMMY_FS_HIGH / DUMMY_FS_IMU;  /* 12 */
    int hi = 0, im = 0;

    while (hi < DUMMY_N_HIGH || im < DUMMY_N_IMU) {
        if (im < DUMMY_N_IMU) {
            mcsigpack_sample_t s = {0};
            s.channel   = MCSIGPACK_CH_IMU;
            s.values[0] = dummy_imu[im][0];
            s.values[1] = dummy_imu[im][1];
            s.values[2] = dummy_imu[im][2];
            mcsigpack_push_sample(ctx, &s);
            im++;
        }
        int batch = HIGH_RATIO;
        while (batch-- > 0 && hi < DUMMY_N_HIGH) {
            mcsigpack_sample_t s = {0};
            s.channel   = MCSIGPACK_CH_EEG1; s.values[0] = dummy_eeg1[hi];
            mcsigpack_push_sample(ctx, &s);
            s.channel   = MCSIGPACK_CH_EEG2; s.values[0] = dummy_eeg2[hi];
            mcsigpack_push_sample(ctx, &s);
            s.channel   = MCSIGPACK_CH_ECG;  s.values[0] = dummy_ecg[hi];
            mcsigpack_push_sample(ctx, &s);
            hi++;
        }
    }
    mcsigpack_flush(ctx);
}

/* =========================================================================
 * Unit tests
 * ====================================================================== */

static void test_init(void)
{
    printf("\n--- init ---\n");
    mcsigpack_ctx_t ctx;
    capture_t cap = {0};

    CHECK(mcsigpack_init(NULL, on_packet, &cap) == MCSIGPACK_ERR_ARGS, "null ctx rejected");
    CHECK(mcsigpack_init(&ctx, NULL, NULL)       == MCSIGPACK_ERR_ARGS, "null fn rejected");
    CHECK(mcsigpack_init(&ctx, on_packet, &cap)  == MCSIGPACK_OK,       "valid init ok");
    CHECK(ctx.initialised,                                               "initialised flag set");
    CHECK(ctx.ch[MCSIGPACK_CH_EEG1].chunk_samples == 250 * MCSIGPACK_CHUNK_SECS,
          "EEG1 chunk_samples correct");
    CHECK(ctx.ch[MCSIGPACK_CH_IMU].chunk_samples == 20 * MCSIGPACK_CHUNK_SECS,
          "IMU chunk_samples correct");
    CHECK(ctx.ch[MCSIGPACK_CH_IMU].n_axes == 3, "IMU n_axes == 3");
}

static void test_epoch_fires(void)
{
    printf("\n--- epoch fires ---\n");
    mcsigpack_ctx_t ctx;
    capture_t cap = {0};
    mcsigpack_init(&ctx, on_packet, &cap);

    fill_epoch(&ctx, 1000);

    CHECK(cap.count > 0, "packets emitted after full epoch");

    for (int i = 0; i < cap.count; i++) {
        int found = 0;
        for (int c = 0; c < MCSIGPACK_NUM_CHANNELS; c++)
            if (ctx.ch[c].wire_id == cap.pkts[i].channel_id) { found = 1; break; }
        CHECK(found, "packet channel_id is a known wire ID");
    }
}

static void test_sequence_numbers(void)
{
    printf("\n--- sequence numbers ---\n");
    mcsigpack_ctx_t ctx;
    capture_t cap = {0};
    mcsigpack_init(&ctx, on_packet, &cap);

    fill_epoch(&ctx, 500);
    fill_epoch(&ctx, 500);

    for (int c = 0; c < MCSIGPACK_NUM_CHANNELS; c++) {
        uint8_t wid = ctx.ch[c].wire_id;
        int last_seq = -1, ok = 1;
        for (int i = 0; i < cap.count; i++) {
            if (cap.pkts[i].channel_id != wid) continue;
            int seq = cap.pkts[i].seq_num;
            if (last_seq >= 0 && seq != ((last_seq + 1) & 0xFF)) { ok = 0; break; }
            last_seq = seq;
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "channel %d seq monotonic", c);
        CHECK(ok, msg);
    }
}

static void test_compression_synthetic(void)
{
    printf("\n--- compression (synthetic signals) ---\n");
    mcsigpack_ctx_t ctx;
    capture_t cap = {0};
    mcsigpack_init(&ctx, on_packet, &cap);

    /* DC signal */
    fill_epoch(&ctx, 2048);
    int ecg_raw     = 250 * MCSIGPACK_CHUNK_SECS * 2;
    int ecg_payload = bytes_for_channel(&cap, ctx.ch[MCSIGPACK_CH_ECG].wire_id);
    printf("  DC    ECG raw=%d  enc=%d  ratio=%.1fx\n",
           ecg_raw, ecg_payload, (float)ecg_raw / ecg_payload);
    CHECK(ecg_payload < ecg_raw, "DC signal compresses");

    /* Ramp signal */
    capture_t cap2 = {0};
    mcsigpack_init(&ctx, on_packet, &cap2);
    int remaining[MCSIGPACK_NUM_CHANNELS];
    int16_t counter[MCSIGPACK_NUM_CHANNELS];
    for (int c = 0; c < MCSIGPACK_NUM_CHANNELS; c++) {
        remaining[c] = k_hz[c] * MCSIGPACK_CHUNK_SECS;
        counter[c]   = 0;
    }
    int any = 1;
    while (any) {
        any = 0;
        for (int c = 0; c < MCSIGPACK_NUM_CHANNELS; c++) {
            if (remaining[c] <= 0) continue;
            mcsigpack_sample_t s; memset(&s, 0, sizeof(s));
            s.channel = (mcsigpack_channel_e)c; s.values[0] = counter[c]++;
            mcsigpack_push_sample(&ctx, &s);
            remaining[c]--; any = 1;
        }
    }
    int ramp_payload = bytes_for_channel(&cap2, ctx.ch[MCSIGPACK_CH_ECG].wire_id);
    printf("  ramp  ECG raw=%d  enc=%d  ratio=%.1fx\n",
           ecg_raw, ramp_payload, (float)ecg_raw / ramp_payload);
    CHECK(ramp_payload < ecg_raw, "ramp signal compresses");
}

static void test_overflow_anchor(void)
{
    printf("\n--- overflow anchor injection ---\n");
    mcsigpack_ctx_t ctx;
    capture_t cap = {0};
    mcsigpack_init(&ctx, on_packet, &cap);

    int remaining[MCSIGPACK_NUM_CHANNELS];
    for (int c = 0; c < MCSIGPACK_NUM_CHANNELS; c++)
        remaining[c] = k_hz[c] * MCSIGPACK_CHUNK_SECS;

    int16_t sign = 1;
    int any = 1;
    while (any) {
        any = 0;
        for (int c = 0; c < MCSIGPACK_NUM_CHANNELS; c++) {
            if (remaining[c] <= 0) continue;
            mcsigpack_sample_t s; memset(&s, 0, sizeof(s));
            s.channel = (mcsigpack_channel_e)c; s.values[0] = sign * 30000;
            sign = (int16_t)(-sign);
            mcsigpack_push_sample(&ctx, &s);
            remaining[c]--; any = 1;
        }
    }

    uint32_t injections = 0;
    for (int c = 0; c < MCSIGPACK_NUM_CHANNELS; c++)
        injections += ctx.ch[c].anchor_injections;

    printf("  anchor injections: %u\n", injections);
    CHECK(cap.count > 0,  "packets emitted on worst-case signal");
    CHECK(injections > 0, "overflow handled via anchor injection");
}

static void test_flush(void)
{
    printf("\n--- partial epoch flush ---\n");
    mcsigpack_ctx_t ctx;
    capture_t cap = {0};
    mcsigpack_init(&ctx, on_packet, &cap);

    int half = (250 * MCSIGPACK_CHUNK_SECS) / 2;
    for (int i = 0; i < half; i++) {
        mcsigpack_sample_t s = {0};
        s.channel = MCSIGPACK_CH_ECG; s.values[0] = (int16_t)i;
        mcsigpack_push_sample(&ctx, &s);
    }

    CHECK(cap.count == 0, "no output before flush");
    mcsigpack_flush(&ctx);
    CHECK(cap.count > 0,  "output emitted after flush");
}

static void test_packet_size(void)
{
    printf("\n--- packet size bounds ---\n");
    mcsigpack_ctx_t ctx;
    capture_t cap = {0};
    mcsigpack_init(&ctx, on_packet, &cap);
    fill_epoch(&ctx, 512);

    int ok = 1;
    for (int i = 0; i < cap.count; i++)
        if (cap.pkts[i].payload_len == 0 ||
            cap.pkts[i].payload_len > MCSIGPACK_PAYLOAD_BYTES) { ok = 0; break; }
    CHECK(ok, "all packets within MCSIGPACK_PAYLOAD_BYTES");
}

/* =========================================================================
 * Benchmarks (real biosignal data from dummy_data.h)
 * ====================================================================== */

static void bench_ratios(void)
{
    printf("\n--- compression ratios (%ds, %d epochs) ---\n",
           DUMMY_DURATION, EXPECTED_EPOCHS);

    mcsigpack_ctx_t ctx;
    capture_t cap = {0};
    mcsigpack_init(&ctx, on_packet, &cap);
    feed_all_data(&ctx);

    printf("\n  channel   raw        encoded    ratio     pkts\n");
    printf("  -------- ---------- ---------- --------- ----\n");

    const struct { const char *name; int ch; int n; int axes; } channels[] = {
        { "EEG1", MCSIGPACK_CH_EEG1, DUMMY_N_HIGH, 1 },
        { "EEG2", MCSIGPACK_CH_EEG2, DUMMY_N_HIGH, 1 },
        { "ECG",  MCSIGPACK_CH_ECG,  DUMMY_N_HIGH, 1 },
        { "IMU",  MCSIGPACK_CH_IMU,  DUMMY_N_IMU,  3 },
    };

    int total_raw = 0, total_enc = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t wid = ctx.ch[channels[i].ch].wire_id;
        int raw     = channels[i].n * channels[i].axes * 2;
        int enc     = bytes_for_channel(&cap, wid);
        int pkts    = 0;
        for (int j = 0; j < cap.count; j++)
            if (cap.pkts[j].channel_id == wid) pkts++;
        printf("  %-8s  raw=%6d B  enc=%6d B  ratio=%5.2fx  %d\n",
               channels[i].name, raw, enc, (double)raw / enc, pkts);
        total_raw += raw; total_enc += enc;
        CHECK(enc < raw, channels[i].name);
    }
    printf("\n  total     raw=%6d B  enc=%6d B  ratio=%5.2fx  %d pkts\n",
           total_raw, total_enc, (double)total_raw / total_enc, cap.count);
    CHECK(!cap.overflow, "capture buffer did not overflow");
}

static void bench_epoch_count(void)
{
    printf("\n--- epoch count ---\n");
    mcsigpack_ctx_t ctx;
    capture_t cap = {0};
    mcsigpack_init(&ctx, on_packet, &cap);
    feed_all_data(&ctx);

    printf("  duration=%ds  chunk=%ds  expected=%d  actual=%u  stalled=%u\n",
           DUMMY_DURATION, MCSIGPACK_CHUNK_SECS, EXPECTED_EPOCHS,
           ctx.epochs_completed, ctx.stalled_samples);

    CHECK((int)ctx.epochs_completed == EXPECTED_EPOCHS, "epoch count correct");
}

static void bench_block_analysis(void)
{
    printf("\n--- block analysis ---\n");
    mcsigpack_ctx_t ctx;
    capture_t cap = {0};
    mcsigpack_init(&ctx, on_packet, &cap);
    feed_all_data(&ctx);

    const struct { const char *name; int ch; int axes; } chans[] = {
        { "EEG1", MCSIGPACK_CH_EEG1, 1 },
        { "EEG2", MCSIGPACK_CH_EEG2, 1 },
        { "ECG",  MCSIGPACK_CH_ECG,  1 },
        { "IMU",  MCSIGPACK_CH_IMU,  3 },
    };

    for (int ci = 0; ci < 4; ci++) {
        uint8_t wid = ctx.ch[chans[ci].ch].wire_id;
        int n_axes  = chans[ci].axes;
        int shift_hist[8] = {0}, blocks = 0, deltas = 0, overflow_anchors = 0;

        for (int i = 0; i < cap.count; i++) {
            if (cap.pkts[i].channel_id != wid) continue;
            const uint8_t *p = cap.pkts[i].payload;
            int len = cap.pkts[i].payload_len, pos = 0;
            while (pos < len) {
                uint8_t hdr   = p[pos++];
                uint8_t shift = (hdr >> 5) & 0x07;
                uint8_t dc    = hdr & 0x1F;
                int bsz = n_axes * 2 + dc * n_axes;
                if (pos + bsz > len) break;
                shift_hist[shift]++; blocks++; deltas += dc;
                if (dc == 0) overflow_anchors++;
                pos += bsz;
            }
        }
        if (!blocks) continue;
        printf("  %-8s  blocks=%4d  avg_deltas=%.1f  overflow_anchors=%d\n",
               chans[ci].name, blocks, (double)deltas / blocks, overflow_anchors);
        printf("           shifts: ");
        for (int s = 0; s < 8; s++)
            if (shift_hist[s]) printf("[%d]=%d ", s, shift_hist[s]);
        printf("\n");
    }
}

static void bench_sequence_continuity(void)
{
    printf("\n--- sequence continuity ---\n");
    mcsigpack_ctx_t ctx;
    capture_t cap = {0};
    mcsigpack_init(&ctx, on_packet, &cap);
    feed_all_data(&ctx);

    for (int c = 0; c < MCSIGPACK_NUM_CHANNELS; c++) {
        uint8_t wid = ctx.ch[c].wire_id;
        int last_seq = -1, ok = 1, count = 0;
        for (int i = 0; i < cap.count; i++) {
            if (cap.pkts[i].channel_id != wid) continue;
            int seq = cap.pkts[i].seq_num;
            if (last_seq >= 0 && seq != ((last_seq + 1) & 0xFF)) { ok = 0; break; }
            last_seq = seq; count++;
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "channel %d seq continuous (%d pkts)", c, count);
        CHECK(ok, msg);
    }
}

/* =========================================================================
 * Main
 * ====================================================================== */
int main(void)
{
    printf("mcsigpack — %d channels  chunk=%ds  MTU=%dB\n",
           MCSIGPACK_NUM_CHANNELS, MCSIGPACK_CHUNK_SECS, MCSIGPACK_MTU);

    printf("\n== unit tests ==\n");
    test_init();
    test_epoch_fires();
    test_sequence_numbers();
    test_compression_synthetic();
    test_overflow_anchor();
    test_flush();
    test_packet_size();

    printf("\n== benchmarks (%ds biosignal data) ==\n", DUMMY_DURATION);
    bench_ratios();
    bench_epoch_count();
    bench_block_analysis();
    bench_sequence_continuity();

    printf("\n%d/%d checks passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
