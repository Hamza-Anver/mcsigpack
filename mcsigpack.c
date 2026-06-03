#include "mcsigpack.h"
#include <string.h>
#include <stddef.h>

/* Channel config tables derived from MCSIGPACK_CHANNEL_LIST. */
static const uint8_t k_wire_id[MCSIGPACK_NUM_CHANNELS] = {
#define X(name, wire, hz, axes) (wire),
    MCSIGPACK_CHANNEL_LIST
#undef X
};

static const uint8_t k_n_axes[MCSIGPACK_NUM_CHANNELS] = {
#define X(name, wire, hz, axes) (axes),
    MCSIGPACK_CHANNEL_LIST
#undef X
};

static const uint16_t k_chunk_samples[MCSIGPACK_NUM_CHANNELS] = {
#define X(name, wire, hz, axes) ((uint16_t)((hz) * MCSIGPACK_CHUNK_SECS)),
    MCSIGPACK_CHANNEL_LIST
#undef X
};

/**
 * Block wire format:
 *   header  : 1 byte  — top 3 bits = shift factor (0-7), bottom 5 = delta count (0-31)
 *   anchor  : n_axes * 2 bytes — absolute int16 LE per axis
 *   deltas  : delta_count * n_axes bytes — right-shifted int8 per axis
 */
#define BLOCK_MAX_DELTAS 31

/**
 * Encode one block of up to 32 samples into out[].
 * @param overflow_at  Set to index of first overflowing sample, or -1.
 * @return Bytes written, or 0 if out_max is too small.
 */
static int encode_block(
    const int16_t samples[][MCSIGPACK_MAX_AXES],
    int count, int n_axes,
    uint8_t *out, int out_max,
    int *overflow_at)
{
    int delta_count = count - 1;
    *overflow_at = -1;

    if (delta_count < 0 || delta_count > BLOCK_MAX_DELTAS) return 0;

    /* Find max absolute delta to select the minimum valid shift. */
    int32_t max_abs = 0;
    for (int s = 1; s < count; s++) {
        for (int a = 0; a < n_axes; a++) {
            int32_t d = (int32_t)samples[s][a] - (int32_t)samples[s-1][a];
            int32_t ad = d < 0 ? -d : d;
            if (ad > max_abs) max_abs = ad;
        }
    }

    uint8_t shift = 0;
    while (shift < 7 && (max_abs >> shift) > 127) shift++;

    if ((max_abs >> shift) > 127) {
        /* Still overflows at shift=7 — find the split point. */
        for (int s = 1; s < count; s++) {
            for (int a = 0; a < n_axes; a++) {
                int32_t d = (int32_t)samples[s][a] - (int32_t)samples[s-1][a];
                int32_t ad = d < 0 ? -d : d;
                if ((ad >> 7) > 127) {
                    *overflow_at = s;
                    delta_count  = s - 1;
                    count        = s;
                    goto done_scan;
                }
            }
        }
        done_scan:;
    }

    int needed = 1 + n_axes * 2 + delta_count * n_axes;
    if (needed > out_max) return 0;

    out[0] = (uint8_t)((shift & 0x07) << 5) | (uint8_t)(delta_count & 0x1F);

    int pos = 1;
    for (int a = 0; a < n_axes; a++) {
        int16_t v = samples[0][a];
        out[pos++] = (uint8_t)(v & 0xFF);
        out[pos++] = (uint8_t)((v >> 8) & 0xFF);
    }

    for (int s = 1; s <= delta_count; s++) {
        for (int a = 0; a < n_axes; a++) {
            int32_t d = (int32_t)samples[s][a] - (int32_t)samples[s-1][a];
            out[pos++] = (int8_t)(d >> shift);
        }
    }

    return pos;
}

/**
 * Encode all buffered samples for one channel into out[].
 * Injects a new anchor on overflow and continues — no data is lost.
 * @return Total bytes written.
 */
static int encode_channel(mcsigpack_ch_state_t *ch, uint8_t *out, int out_max)
{
    int total  = (int)ch->buf_head;
    int written = 0;
    int src     = 0;

    while (src < total) {
        int count = total - src;
        if (count > BLOCK_MAX_DELTAS + 1) count = BLOCK_MAX_DELTAS + 1;

        int overflow_at = -1;
        int bytes = encode_block(
            (const int16_t (*)[MCSIGPACK_MAX_AXES])&ch->buf[src],
            count, ch->n_axes,
            out + written, out_max - written,
            &overflow_at);

        if (bytes == 0) break;

        written += bytes;

        if (overflow_at > 0) {
            src += overflow_at;
            ch->anchor_injections++;
        } else {
            src += count;
        }
    }

    return written;
}

/** Split encoded bytes into MTU-sized packets and call output_fn for each. */
static void pack_and_emit(mcsigpack_ctx_t *ctx, uint8_t ch_idx,
                          const uint8_t *encoded, int encoded_len)
{
    mcsigpack_ch_state_t *ch = &ctx->ch[ch_idx];
    int pos = 0;

    while (pos < encoded_len) {
        mcsigpack_packet_t pkt;
        pkt.channel_id  = ch->wire_id;
        pkt.seq_num     = ch->seq++;
        pkt.payload_len = 0;

        int to_copy = encoded_len - pos;
        if (to_copy > (int)MCSIGPACK_PAYLOAD_BYTES) to_copy = MCSIGPACK_PAYLOAD_BYTES;

        memcpy(pkt.payload, encoded + pos, (size_t)to_copy);
        pkt.payload_len = (uint8_t)to_copy;
        pos += to_copy;

        ctx->output_fn(&pkt, ctx->output_user_data);
    }
}

/* Worst-case encoded size: every sample is its own anchor block. */
#define ENCODE_SCRATCH_BYTES (MCSIGPACK_MAX_CHUNK_SAMPLES * (1 + MCSIGPACK_MAX_AXES * 2))

static void flush_epoch(mcsigpack_ctx_t *ctx)
{
    static uint8_t scratch[ENCODE_SCRATCH_BYTES];
    int any_encoded = 0;

    for (int i = 0; i < MCSIGPACK_NUM_CHANNELS; i++) {
        mcsigpack_ch_state_t *ch = &ctx->ch[i];
        if (ch->buf_head == 0) continue;

        int encoded = encode_channel(ch, scratch, (int)sizeof(scratch));
        if (encoded > 0) {
            pack_and_emit(ctx, (uint8_t)i, scratch, encoded);
            ch->epochs_encoded++;
            any_encoded = 1;
        }

        ch->buf_head  = 0;
        ch->remaining = ch->chunk_samples;
    }

    if (any_encoded) ctx->epochs_completed++;
}

static bool all_channels_done(const mcsigpack_ctx_t *ctx)
{
    for (int i = 0; i < MCSIGPACK_NUM_CHANNELS; i++)
        if (ctx->ch[i].remaining > 0) return false;
    return true;
}

int mcsigpack_init(mcsigpack_ctx_t *ctx, mcsigpack_output_fn output_fn, void *user_data)
{
    if (!ctx || !output_fn) return MCSIGPACK_ERR_ARGS;

    memset(ctx, 0, sizeof(*ctx));

    for (int i = 0; i < MCSIGPACK_NUM_CHANNELS; i++) {
        ctx->ch[i].wire_id       = k_wire_id[i];
        ctx->ch[i].n_axes        = k_n_axes[i];
        ctx->ch[i].chunk_samples = k_chunk_samples[i];
        ctx->ch[i].remaining     = k_chunk_samples[i];
    }

    ctx->output_fn        = output_fn;
    ctx->output_user_data = user_data;
    ctx->initialised      = true;

    return MCSIGPACK_OK;
}

int mcsigpack_push_sample(mcsigpack_ctx_t *ctx, const mcsigpack_sample_t *sample)
{
    if (!ctx || !ctx->initialised || !sample) return MCSIGPACK_ERR_ARGS;

    int ch_idx = (int)sample->channel;
    if (ch_idx < 0 || ch_idx >= MCSIGPACK_NUM_CHANNELS) return MCSIGPACK_ERR_ARGS;

    mcsigpack_ch_state_t *ch = &ctx->ch[ch_idx];

    if (ch->remaining == 0) {
        ctx->stalled_samples++;
        return MCSIGPACK_ERR_OVERFLOW;
    }

    memcpy(ch->buf[ch->buf_head], sample->values, sizeof(sample->values));
    ch->buf_head++;
    ch->remaining--;

    if (all_channels_done(ctx))
        flush_epoch(ctx);

    return MCSIGPACK_OK;
}

void mcsigpack_flush(mcsigpack_ctx_t *ctx)
{
    if (!ctx || !ctx->initialised) return;
    flush_epoch(ctx);
}
