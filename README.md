# mcsigpack

Multichannel biosignal compression for BLE streaming. Takes raw ECG, EEG, and IMU samples and packs them into BLE GATT packets using delta encoding.

Pure C99. No RTOS or heap dependency. Works standalone or as a Zephyr module.

## How it works

Samples are collected per channel until every channel has filled its chunk window (default 5s). At that point the chunk is encoded and handed off.

Each chunk is encoded as a series of blocks:

| Field | Size | Description |
|---|---|---|
| Header | 1 byte | Top 3 bits: shift factor (0-7). Bottom 5 bits: delta count (0-31) |
| Anchor | n_axes x 2 bytes | Absolute sample value, int16 little-endian |
| Deltas | delta_count x n_axes bytes | Difference from previous sample, right-shifted to fit int8 |

The shift factor is chosen per block to be the smallest value that fits the largest delta into 8 bits. If a spike is too large even at shift=7 (QRS complexes in ECG, for example), the block is cut short and a fresh anchor is injected. No data is lost.

Encoded blocks are split into 247-byte MTU packets with a 2-byte header (channel ID + sequence number) and passed to the output callback.

## Compression results

Measured on 5s of synthetic biosignals at 247-byte MTU:

| Channel | Raw | Encoded | Ratio |
|---|---|---|---|
| EEG1 (250 Hz) | 2500 B | 1330 B | 1.88x |
| EEG2 (250 Hz) | 2500 B | 1330 B | 1.88x |
| ECG (250 Hz) | 2500 B | 1330 B | 1.88x |
| IMU (20 Hz, 3-axis) | 600 B | 316 B | 1.90x |
| **Total** | **8100 B** | **4306 B** | **1.88x** |

About 1.9x is the ceiling for this approach since you are going from 16-bit samples down to 8-bit deltas. Adding entropy coding (Rice/Golomb) on top of the deltas would push this further.

### Shift Factor Impact

Measured by setting the same max shift on every channel.

| Channel | Ratio @ 0 | MAE @ 0 | Ratio @ 7 | MAE @ 7 |
|---|---:|---:|---:|---:|
| EEG1 | 1.55x | 0.00 (0.0%) | 1.88x | 9.99 (4.6%) |
| EEG2 | 1.07x | 0.00 (0.0%) | 1.88x | 18.66 (8.5%) |
| ECG | 1.80x | 0.00 (0.0%) | 1.88x | 1.99 (0.9%) |
| IMU | 0.86x | 0.00 (0.0%) | 1.90x | 22.57 (7.7%) |

## Usage

Define your channels in `mcsigpack.h`:

```c
#define MCSIGPACK_CHANNEL_LIST       \
    X(MCSIGPACK_CH_EEG1, 0, 250, 1, 7) \
    X(MCSIGPACK_CH_EEG2, 1, 250, 1, 7) \
    X(MCSIGPACK_CH_ECG,  2, 250, 1, 7) \
    X(MCSIGPACK_CH_IMU,  3,  20, 3, 7)
//   enum name, wire ID, Hz, axes, max_shift
```

Then initialise and push samples:

```c
static void my_output(const mcsigpack_packet_t *pkt, void *ud) {
    // forward to your BLE queue
    // on Zephyr: k_msgq_put(out_q, pkt, K_NO_WAIT);
}

mcsigpack_ctx_t ctx;
mcsigpack_init(&ctx, my_output, NULL);

mcsigpack_sample_t s = { .channel = MCSIGPACK_CH_ECG, .values = { raw_sample } };
mcsigpack_push_sample(&ctx, &s);
```

Call `mcsigpack_flush()` on shutdown or if a sensor drops out.

## Zephyr integration

Add to your `west.yml`:

```yaml
- name: mcsigpack
  url: https://github.com/yourorg/mcsigpack
  path: modules/mcsigpack
  revision: main
```

Enable in `prj.conf`:

```
CONFIG_MCSIGPACK=y
```

Link in your app `CMakeLists.txt`:

```cmake
target_link_libraries(app PRIVATE mcsigpack)
```

Example Zephyr shim:

```c
// app/src/mcsigpack_zephyr.c  — lives in YOUR app, not the library

#include <zephyr/kernel.h>
#include "mcsigpack.h"

// These are yours to define — stack size, priority, queue depth
#define STACK_SIZE  2048
#define PRIORITY    5
#define QUEUE_DEPTH 16

K_THREAD_STACK_DEFINE(mcsigpack_stack, STACK_SIZE);
K_MSGQ_DEFINE(mcsigpack_out_q, sizeof(mcsigpack_packet_t), QUEUE_DEPTH, 4);

static struct k_thread mcsigpack_thread_data;
static mcsigpack_ctx_t mcsigpack_ctx;

static void output_cb(const mcsigpack_packet_t *pkt, void *ud)
{
    // non-blocking put — drops if queue is full, which is the right
    // behaviour on a sensor device rather than blocking the encoder
    k_msgq_put(&mcsigpack_out_q, pkt, K_NO_WAIT);
}

static void mcsigpack_thread(void *p1, void *p2, void *p3)
{
    // your sensor sample arrives via a separate input queue
    struct k_msgq *in_q = (struct k_msgq *)p1;
    mcsigpack_sample_t sample;

    while (1) {
        k_msgq_get(in_q, &sample, K_FOREVER);
        mcsigpack_push_sample(&mcsigpack_ctx, &sample);
    }
}

void mcsigpack_zephyr_init(struct k_msgq *in_q)
{
    mcsigpack_init(&mcsigpack_ctx, output_cb, NULL);

    k_thread_create(&mcsigpack_thread_data, mcsigpack_stack,
                    K_THREAD_STACK_SIZEOF(mcsigpack_stack),
                    mcsigpack_thread, in_q, NULL, NULL,
                    PRIORITY, 0, K_NO_WAIT);

    k_thread_name_set(&mcsigpack_thread_data, "mcsigpack");
}
```

## Testing

Requires a C99 compiler. No other dependencies.

```
make test
```

To regenerate the biosignal test vectors (requires `neurokit2` and `numpy`):

```
python3 tools/gen_dummy_data.py > test/dummy_data.h
```

To test the example Python decompressor against the C harness output:

```bash
python3 tools/test_mcsigpack_decompress.py
```

The script runs the C harness in `--dump` mode, captures the compressed packet stream, and decodes the concatenated per-channel payloads back into absolute samples.

## Structure

```
mcsigpack/
├── Makefile
├── mcsigpack.c
├── mcsigpack.h
├── README.md
├── test
│   ├── dummy_data.h
│   └── test_mcsigpack.c
├── tools
│   ├── mcsigpack_decompress.py
│   ├── test_mcsigpack_decompress.py
│   └── gen_dummy_data.py
└── zephyr
    ├── CMakeLists.txt
    ├── Kconfig
    └── module.yml
```