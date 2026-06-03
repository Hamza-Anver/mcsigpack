#!/usr/bin/env python3

"""Round-trip test for the exported mcsigpack packet fixture."""

from pathlib import Path
import re
import subprocess
import tempfile

from mcsigpack_decompress import decode_payload


CHANNEL_NAMES = {
    0: "EEG1",
    1: "EEG2",
    2: "ECG",
    3: "IMU",
}

CHANNEL_AXES = {
    0: 1,
    1: 1,
    2: 1,
    3: 3,
}

CHANNEL_ARRAYS = {
    0: "dummy_eeg1",
    1: "dummy_eeg2",
    2: "dummy_ecg",
    3: "dummy_imu",
}


def read_constants(header_path: Path) -> dict[str, int]:
    """Read the dummy data sizes from the generated C header."""

    text = header_path.read_text()

    def read_define(name: str) -> int:
        match = re.search(rf"#define\s+{name}\s+(\d+)", text)
        if not match:
            raise ValueError(f"missing {name}")
        return int(match.group(1))

    return {
        "fs_high": read_define("DUMMY_FS_HIGH"),
        "fs_imu": read_define("DUMMY_FS_IMU"),
        "duration": read_define("DUMMY_DURATION"),
    }


def parse_ints(text: str) -> list[int]:
    """Extract signed integers from a block of C initializer text."""

    return [int(value) for value in re.findall(r"-?\d+", text)]


def read_expected_samples(header_path: Path) -> dict[int, list[list[int]]]:
    """Read the generated dummy sample arrays from the C header."""

    text = header_path.read_text()
    expected: dict[int, list[list[int]]] = {}

    for channel_id, array_name in CHANNEL_ARRAYS.items():
        match = re.search(
            rf"static const int16_t {array_name}\[[^\]]+\](?:\[[^\]]+\])? = \{{(.*?)\}};",
            text,
            re.S,
        )
        if not match:
            raise ValueError(f"missing {array_name}")

        body = match.group(1)
        if channel_id == 3:
            rows = []
            for row in re.findall(r"\{([^{}]+)\}", body):
                values = parse_ints(row)
                if len(values) == CHANNEL_AXES[channel_id]:
                    rows.append(values)
            expected[channel_id] = rows
        else:
            expected[channel_id] = [[value] for value in parse_ints(body)]

    return expected


def load_fixture(path: Path) -> list[bytes]:
    """Split the exported packet stream into packet records."""

    data = path.read_bytes()
    if data[:5] != b"MCPK\x01":
        raise ValueError("bad fixture header")

    packets: list[bytes] = []
    pos = 5
    while pos < len(data):
        if pos + 3 > len(data):
            raise ValueError("truncated packet record")
        payload_len = data[pos + 2]
        end = pos + 3 + payload_len
        if end > len(data):
            raise ValueError("truncated packet payload")
        packets.append(data[pos:end])
        pos = end
    return packets


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    binary = root / "out" / "test_mcsigpack"
    header = root / "test" / "dummy_data.h"

    expected_samples = read_expected_samples(header)

    with tempfile.TemporaryDirectory() as tmpdir:
        dump_path = Path(tmpdir) / "mcsigpack_packets.bin"
        subprocess.run([str(binary), "--dump", str(dump_path)], check=True)

        packets = load_fixture(dump_path)
        payloads: dict[int, bytearray] = {}
        packet_counts: dict[int, int] = {}

        for packet in packets:
            channel_id = packet[0]
            payload_len = packet[2]
            payloads.setdefault(channel_id, bytearray()).extend(packet[3:3 + payload_len])
            packet_counts[channel_id] = packet_counts.get(channel_id, 0) + 1

        for channel_id in sorted(payloads):
            payload = bytes(payloads[channel_id])
            samples = decode_payload(payload, CHANNEL_AXES[channel_id])
            expected = expected_samples[channel_id]
            mismatches = abs(len(samples) - len(expected))
            abs_error_sum = 0
            abs_ref_sum = 0
            compared_values = 0
            for decoded, reference in zip(samples, expected):
                if decoded != reference:
                    mismatches += 1
                for decoded_value, reference_value in zip(decoded, reference):
                    error = abs(decoded_value - reference_value)
                    abs_error_sum += error
                    abs_ref = abs(reference_value)
                    abs_ref_sum += abs_ref
                    compared_values += 1

            mean_abs_error = (abs_error_sum / compared_values) if compared_values else 0.0
            mean_abs_reference = (abs_ref_sum / compared_values) if compared_values else 0.0
            mean_abs_error_pct = (
                (mean_abs_error / mean_abs_reference) * 100.0 if mean_abs_reference else 0.0
            )

            print(
                f"{CHANNEL_NAMES[channel_id]}: samples [{len(samples)}/{len(expected)}], "
                f"mismatches={mismatches}, mae={mean_abs_error:.2f} ({mean_abs_error_pct:.1f}%)"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())