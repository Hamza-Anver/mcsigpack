#!/usr/bin/env python3

"""Decode mcsigpack block streams."""

import struct


def decode_payload(payload: bytes, n_axes: int) -> list[list[int]]:
    """Decode a concatenated block payload stream into absolute samples."""

    samples: list[list[int]] = []
    pos = 0

    while pos < len(payload):
        header = payload[pos]
        pos += 1
        shift = (header >> 5) & 0x07
        delta_count = header & 0x1F

        anchor_fmt = "<" + "h" * n_axes
        anchor = list(struct.unpack_from(anchor_fmt, payload, pos))
        pos += n_axes * 2
        samples.append(anchor)

        prev = anchor
        for _ in range(delta_count):
            delta_fmt = "<" + "b" * n_axes
            deltas = struct.unpack_from(delta_fmt, payload, pos)
            pos += n_axes
            sample = [prev[i] + (deltas[i] << shift) for i in range(n_axes)]
            samples.append(sample)
            prev = sample

    return samples


__all__ = ["decode_payload"]
