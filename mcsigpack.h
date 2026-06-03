/**
 * @file mcsigpack.h
 * @brief Multichannel biosignal compression for BLE streaming.
 *
 * Pure C99, no RTOS dependency. I/O is injected via output callback.
 *
 * Quick start:
 *  1. Edit MCSIGPACK_CHANNEL_LIST to match your channels.
 *  2. Call mcsigpack_init() with an output callback.
 *  3. Call mcsigpack_push_sample() for each incoming sample.
 *  4. The callback receives completed BLE-ready packets.
 *
 * Zephyr adapter example:
 *  @code
 *  static void zephyr_out(const mcsigpack_packet_t *pkt, void *ud) {
 *      k_msgq_put((struct k_msgq *)ud, pkt, K_NO_WAIT);
 *  }
 *  @endcode
 */

#ifndef MCSIGPACK_H
#define MCSIGPACK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * @defgroup config Channel configuration
 * @{
 *
 * Edit MCSIGPACK_CHANNEL_LIST to add/remove channels. Everything else derives
 * from this table automatically — enums, buffer sizes, chunk counters.
 *
 * Column order: X(enum_name, wire_id, hz, n_axes)
 *   - wire_id : 0-255, written into the packet channel byte
 *   - hz      : sample rate in Hz
 *   - n_axes  : samples per frame (e.g. 1 for ECG/EEG, 3 for IMU)
 */
#define MCSIGPACK_CHUNK_SECS 5

#define MCSIGPACK_CHANNEL_LIST      \
    X(MCSIGPACK_CH_EEG1, 0, 250, 1) \
    X(MCSIGPACK_CH_EEG2, 1, 250, 1) \
    X(MCSIGPACK_CH_ECG,  2, 250, 1) \
    X(MCSIGPACK_CH_IMU,  3,  20, 3)
/** @} */

/** Channel enum — auto-generated from MCSIGPACK_CHANNEL_LIST. */
typedef enum {
#define X(name, wire, hz, axes) name,
    MCSIGPACK_CHANNEL_LIST
#undef X
    MCSIGPACK_NUM_CHANNELS
} mcsigpack_channel_e;

#define MCSIGPACK_MAX_AXES          3
#define MCSIGPACK_MAX_HZ            250
#define MCSIGPACK_MAX_CHUNK_SAMPLES (MCSIGPACK_CHUNK_SECS * MCSIGPACK_MAX_HZ)

_Static_assert(MCSIGPACK_NUM_CHANNELS <= 32, "too many channels");
_Static_assert(MCSIGPACK_MAX_AXES     <= 3,  "block format supports max 3 axes");
_Static_assert(MCSIGPACK_CHUNK_SECS   >= 1,  "chunk duration must be >= 1s");

/**
 * @defgroup packet BLE packet
 *
 * Wire layout: [channel : 1B][seq : 1B][blocks : up to MCSIGPACK_PAYLOAD_BYTES]
 *
 * Sized for 247-byte ATT_MTU (BLE 4.2+ with Data Length Extension).
 * Adjust MCSIGPACK_MTU for a different MTU.
 * @{
 */
#define MCSIGPACK_MTU           247
#define MCSIGPACK_HDR_BYTES     2
#define MCSIGPACK_PAYLOAD_BYTES (MCSIGPACK_MTU - MCSIGPACK_HDR_BYTES)

/** A fully packed, ready-to-send BLE packet. */
typedef struct {
    uint8_t channel_id;
    uint8_t seq_num;
    uint8_t payload[MCSIGPACK_PAYLOAD_BYTES];
    uint8_t payload_len; /**< Bytes actually used in payload[]. */
} mcsigpack_packet_t;
/** @} */

/** One input sample. Set unused axes to 0. */
typedef struct {
    mcsigpack_channel_e channel;
    int16_t             values[MCSIGPACK_MAX_AXES];
} mcsigpack_sample_t;

/**
 * Output callback, called once per completed packet.
 * Must be non-blocking. Called from whichever thread calls mcsigpack_push_sample().
 */
typedef void (*mcsigpack_output_fn)(const mcsigpack_packet_t *packet, void *user_data);

/** Per-channel runtime state. */
typedef struct {
    uint8_t  wire_id;
    uint8_t  n_axes;
    uint16_t chunk_samples;
    int16_t  buf[MCSIGPACK_MAX_CHUNK_SAMPLES][MCSIGPACK_MAX_AXES];
    uint16_t buf_head;
    uint16_t remaining;
    uint8_t  seq;
    uint32_t epochs_encoded;
    uint32_t anchor_injections; /**< Mid-block overflow events (diagnostic). */
} mcsigpack_ch_state_t;

/** Library context. Caller owns the memory; treat as opaque. */
typedef struct {
    mcsigpack_ch_state_t ch[MCSIGPACK_NUM_CHANNELS];
    mcsigpack_output_fn  output_fn;
    void                *output_user_data;
    bool                 initialised;
    uint32_t             epochs_completed;
    uint32_t             stalled_samples; /**< Samples dropped while a channel was ahead (diagnostic). */
} mcsigpack_ctx_t;

#define MCSIGPACK_OK            0
#define MCSIGPACK_ERR_ARGS     -1
#define MCSIGPACK_ERR_OVERFLOW -2

/**
 * @brief Initialise the library context.
 * @param ctx        Caller-allocated context.
 * @param output_fn  Called for each completed packet; must be non-blocking.
 * @param user_data  Passed through to output_fn unchanged.
 * @return MCSIGPACK_OK or MCSIGPACK_ERR_ARGS.
 */
int mcsigpack_init(mcsigpack_ctx_t *ctx, mcsigpack_output_fn output_fn, void *user_data);

/**
 * @brief Push one sample into the library.
 *
 * Not thread-safe. Call from a single thread or protect with a mutex.
 * Triggers output_fn when all channels complete their chunk quota.
 *
 * @return MCSIGPACK_OK, MCSIGPACK_ERR_ARGS, or MCSIGPACK_ERR_OVERFLOW.
 */
int mcsigpack_push_sample(mcsigpack_ctx_t *ctx, const mcsigpack_sample_t *sample);

/**
 * @brief Flush buffered data regardless of epoch completion.
 *
 * Use on shutdown or sensor dropout. Channels with no buffered data are skipped.
 */
void mcsigpack_flush(mcsigpack_ctx_t *ctx);

#endif /* MCSIGPACK_H */
