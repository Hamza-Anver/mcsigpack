# mcsigpack

Multichannel biosignal compression for BLE streaming. Compresses ECG, EEG, and IMU data using adaptive delta encoding before transmission over BLE GATT.

- Pure C99 — no RTOS or heap dependency
- Drop-in Zephyr module via west
- Output injected via callback — works with any queue or transport

## How it works

Samples are buffered per channel until all channels complete a chunk (default 5 s). The chunk is then encoded as a series of blocks: each block stores one absolute anchor sample followed by up to 31 right-shifted 8-bit deltas. The shift factor is chosen per block to keep all deltas in range. Encoded bytes are split into 247-byte BLE MTU packets and handed to the output callback.

## Compression results

Measured on 5 s of synthetic biosignals (250 Hz EEG/ECG, 20 Hz IMU, 247-byte MTU):

```
channel   raw        encoded    ratio
EEG1      2500 B     1330 B     1.88x
EEG2      2500 B     1330 B     1.88x
ECG       2500 B     1330 B     1.88x
IMU        600 B      316 B     1.90x
total     8100 B     4306 B     1.88x
```

~1.9x is the ceiling for single-pass 16→8 bit delta encoding without entropy coding. ECG shows a bimodal shift distribution — slow baseline at shift=0, QRS spikes at shift=7 — confirming the per-block adaptation is working.

## Usage

Edit `MCSIGPACK_CHANNEL_LIST` in `include/mcsigpack.h`:

```c
#define MCSIGPACK_CHANNEL_LIST       \
    X(MCSIGPACK_CH_EEG1, 0, 250, 1) \
    X(MCSIGPACK_CH_EEG2, 1, 250, 1) \
    X(MCSIGPACK_CH_ECG,  2, 250, 1) \
    X(MCSIGPACK_CH_IMU,  3,  20, 3)
```

Then initialise and feed samples:

```c
static void my_output(const mcsigpack_packet_t *pkt, void *ud) {
    /* forward to BLE queue, e.g. k_msgq_put(out_q, pkt, K_NO_WAIT) */
}

mcsigpack_ctx_t ctx;
mcsigpack_init(&ctx, my_output, NULL);

mcsigpack_sample_t s = { .channel = MCSIGPACK_CH_ECG, .values = { raw_sample } };
mcsigpack_push_sample(&ctx, &s);
```

Call `mcsigpack_flush()` on shutdown or sensor dropout to emit whatever is buffered.

## Zephyr integration

Add to your firmware's `west.yml`:

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

Link in your app's `CMakeLists.txt`:

```cmake
target_link_libraries(app PRIVATE mcsigpack)
```

## Testing

Requires a C99 compiler with AddressSanitizer. No other dependencies for the tests.

```
make test
```

To regenerate the biosignal test vectors (requires `neurokit2` and `numpy`):

```
python3 tools/gen_dummy_data.py > test/dummy_data.h
```

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
│   └── gen_dummy_data.py
└── zephyr
    ├── CMakeLists.txt
    ├── Kconfig
    └── module.yml
```
