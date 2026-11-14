#!/usr/bin/env python3
"""Synthesize a .sr capture containing known UART bytes (8N1, LSB first).

Usage: make_uart_sr.py OUT.sr RATE BAUD BYTES...
Example: make_uart_sr.py uart.sr 1000000 115200 0x55 0x33
"""
import io
import struct
import sys
import zipfile


def uart_bits(byte, baud, rate):
    """Return list of (level, samples) for 8N1 LSB-first on the TX line."""
    spb = rate // baud  # samples per bit
    frames = [(0, spb)]              # start bit
    for i in range(8):
        frames.append(((byte >> i) & 1, spb))
    frames.append((1, spb))          # stop bit
    return frames


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    out, rate, baud = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    payload = [int(b, 0) for b in sys.argv[4:]]

    samples = []
    spb = rate // baud
    idle_lead = spb * 10          # idle-high prefix: gives the decoder a
                                  # falling edge to sync on
    samples.append((1, idle_lead))
    for byte in payload:
        samples.extend(uart_bits(byte, baud, rate))
    samples.append((1, spb * 10))  # idle tail
    total = sum(n for _, n in samples)

    # One byte per sample; unitsize=1, so bit0 of each byte = channel D0.
    nsamples = total
    bits = [1] * nsamples  # idle high
    pos = 0
    for level, n in samples:
        for _ in range(n):
            bits[pos] = level
            pos += 1

    data = bytearray()
    for level in bits:
        data.append(1 if level else 0)

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as z:
        z.writestr("version", "2")
        z.writestr("metadata",
            "[global]\n"
            "sigrok version=0.6.0\n"
            "\n"
            "[device 1]\n"
            "capturefile=logic-1\n"
            "total probes=1\n"
            f"samplerate={rate} Hz\n"
            "total analog=0\n"
            "probe1=D0\n"
            "unitsize=1\n")
        z.writestr("logic-1-1", bytes(data))
    with open(out, "wb") as f:
        f.write(buf.getvalue())
    print(f"wrote {out}: {nsamples} samples @ {rate} Hz, "
          f"{len(payload)} UART bytes @ {baud} baud")


if __name__ == "__main__":
    main()
