#!/usr/bin/env python3
"""Summarize an Android Auto frame capture taken by aawgd (AAWG_CAPTURE_FRAMES=1).

Payloads are encrypted between phone and head unit, so this works from the frame
index: which channels appear, in what order, with which sizes. Frames that are
still plaintext (everything before the TLS handshake, plus pings) get decoded.

Usage: capture_report.py <capture-from-phone.idx.csv> [capture-from-headunit.idx.csv]
"""
import csv
import sys

FRAME_TYPES = {0b00: "MIDDLE", 0b01: "FIRST", 0b10: "LAST", 0b11: "BULK"}
INDEX_FIELDS = [
    "ts_ms", "dir", "offset", "channel", "flags", "frame_type", "msg_type",
    "enc", "header_len", "payload_len", "total_msg_size", "payload_hex",
]
CONTROL_MSG_NAMES = {
    0x0001: "VERSION_REQUEST",
    0x0002: "VERSION_RESPONSE",
    0x0003: "SSL_HANDSHAKE",
    0x0004: "AUTH_COMPLETE",
    0x0005: "SERVICE_DISCOVERY_REQUEST",
    0x0006: "SERVICE_DISCOVERY_RESPONSE",
    0x0007: "CHANNEL_OPEN_REQUEST",
    0x0008: "CHANNEL_OPEN_RESPONSE",
    0x000B: "PING_REQUEST",
    0x000C: "PING_RESPONSE",
    0x000F: "SHUTDOWN_REQUEST",
    0x0010: "SHUTDOWN_RESPONSE",
}


def read_index(path):
    rows = []
    with open(path) as handle:
        reader = csv.DictReader((line for line in handle if not line.startswith("#")), fieldnames=INDEX_FIELDS)
        for row in reader:
            if row["channel"] == "UNPARSED":
                continue
            row["ts_ms"] = int(row["ts_ms"])
            row["offset"] = int(row["offset"])
            row["channel"] = int(row["channel"])
            row["flags"] = int(row["flags"], 16)
            row["payload_len"] = int(row["payload_len"])
            row["total_msg_size"] = int(row["total_msg_size"])
            rows.append(row)
    return rows


def decode_plaintext(row):
    """Return a short description for control-channel frames that are still plaintext."""
    if row["enc"] != "0" or row["channel"] != 0:
        return ""

    hex_payload = row["payload_hex"]
    if len(hex_payload) < 4:
        return ""

    msg_id = int(hex_payload[0:4], 16)
    name = CONTROL_MSG_NAMES.get(msg_id, f"control_0x{msg_id:04x}")

    if msg_id in (0x0001, 0x0002) and len(hex_payload) >= 12:
        major = int(hex_payload[4:8], 16)
        minor = int(hex_payload[8:12], 16)
        status = int(hex_payload[12:16], 16) if len(hex_payload) >= 16 else None
        extra = f" v{major}.{minor}" + (f" status={status}" if status is not None else "")
        return name + extra

    return name


def summarize(rows):
    print(f"== {len(rows)} frames, {rows[-1]['ts_ms']} ms ==")

    per_channel = {}
    for row in rows:
        stats = per_channel.setdefault(row["channel"], {"count": 0, "bytes": 0, "first_ts": row["ts_ms"], "last_ts": row["ts_ms"]})
        stats["count"] += 1
        stats["bytes"] += row["payload_len"]
        stats["last_ts"] = row["ts_ms"]

    print(f"{'ch':>4} {'frames':>7} {'bytes':>9} {'first_ms':>9} {'last_ms':>9}")
    for channel in sorted(per_channel):
        stats = per_channel[channel]
        print(f"{channel:>4} {stats['count']:>7} {stats['bytes']:>9} {stats['first_ts']:>9} {stats['last_ts']:>9}")

    print("-- first 25 frames --")
    for row in rows[:25]:
        note = decode_plaintext(row)
        print(
            f"t+{row['ts_ms']:>6}ms ch{row['channel']:<3} {row['frame_type']:<6} "
            f"enc={row['enc']} len={row['payload_len']:<6} total={row['total_msg_size']:<7} {note}"
        )

    fragmented = sum(1 for row in rows if row["frame_type"] in ("FIRST", "MIDDLE", "LAST"))
    print(f"-- {fragmented} fragmented frames (messages above the frame size limit) --")


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1

    for path in argv[1:]:
        print(f"\n##### {path}")
        rows = read_index(path)
        if not rows:
            print("no parsed frames")
            continue
        summarize(rows)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
