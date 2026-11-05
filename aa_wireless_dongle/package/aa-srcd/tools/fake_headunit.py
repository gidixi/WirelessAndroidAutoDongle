#!/usr/bin/env python3
"""Minimal Android Auto head unit, used to bench aa-srcd without a car.

It plays the head unit side of the protocol: version exchange, TLS client
handshake carried inside control messages, auth, service discovery, channel
open, AV setup/start, then it receives video frames and ACKs them.

Usage:
    ./aa-srcd --tcp 5288 --h264 dashboard.h264 --fps 5
    ./fake_headunit.py --port 5288 --cert hu.crt --key hu.key --ca srcd.crt --frames 30
"""
import argparse
import socket
import ssl
import struct
import sys

# Control channel message ids
VERSION_REQUEST = 0x0001
VERSION_RESPONSE = 0x0002
SSL_HANDSHAKE = 0x0003
AUTH_COMPLETE = 0x0004
SERVICE_DISCOVERY_REQUEST = 0x0005
SERVICE_DISCOVERY_RESPONSE = 0x0006
CHANNEL_OPEN_REQUEST = 0x0007
CHANNEL_OPEN_RESPONSE = 0x0008
PING_REQUEST = 0x000B
PING_RESPONSE = 0x000C
AV_MEDIA_WITH_TIMESTAMP_INDICATION = 0x0000
SENSOR_START_REQUEST = 0x8001
SENSOR_START_RESPONSE = 0x8002
SENSOR_EVENT_INDICATION = 0x8003

AV_SETUP_REQUEST = 0x8000
AV_START_INDICATION = 0x8001
AV_SETUP_RESPONSE = 0x8003
AV_MEDIA_ACK_INDICATION = 0x8004

FRAME_BULK = 0x03
FRAME_FIRST = 0x01
FRAME_MIDDLE = 0x00
FRAME_LAST = 0x02
FLAG_CONTROL = 0x04
FLAG_ENCRYPTED = 0x08


def tag(field, wire):
    return bytes([((field << 3) | wire)])


def varint(value):
    out = bytearray()
    while True:
        chunk = value & 0x7F
        value >>= 7
        out.append(chunk | (0x80 if value else 0))
        if not value:
            return bytes(out)


def field_varint(number, value):
    return tag(number, 0) + varint(value)


def field_bytes(number, data):
    return tag(number, 2) + varint(len(data)) + data


class TlsLayer:
    """TLS 1.2 over memory BIOs: bytes travel inside AA control messages."""

    def __init__(self, context):
        self.in_bio = ssl.MemoryBIO()
        self.out_bio = ssl.MemoryBIO()
        self.obj = context.wrap_bio(self.in_bio, self.out_bio, server_side=False)
        self.handshake_done = False

    def step(self, incoming=b""):
        if incoming:
            self.in_bio.write(incoming)

        if not self.handshake_done:
            try:
                self.obj.do_handshake()
                self.handshake_done = True
            except ssl.SSLWantReadError:
                pass

        return self.out_bio.read()

    def encrypt(self, data):
        self.obj.write(data)
        return self.out_bio.read()

    def decrypt(self, data):
        self.in_bio.write(data)
        plain = b""
        while True:
            try:
                chunk = self.obj.read(65536)
            except ssl.SSLWantReadError:
                return plain
            if not chunk:
                return plain
            plain += chunk


class HeadUnit:
    def __init__(self, sock, tls):
        self.sock = sock
        self.tls = tls
        self.rx = b""
        self.fragments = b""
        self.fragment_meta = None
        self.pending = []

    def send_frame(self, channel, flags, payload):
        header = bytes([channel, flags]) + struct.pack(">H", len(payload))
        if (flags & 0x03) == FRAME_FIRST:
            header += struct.pack(">I", len(payload))
        self.sock.sendall(header + payload)

    def send_message(self, channel, message_id, body=b"", control=True, encrypted=False):
        payload = struct.pack(">H", message_id) + body
        if encrypted:
            payload = self.tls.encrypt(payload)

        flags = FRAME_BULK | (FLAG_CONTROL if control else 0) | (FLAG_ENCRYPTED if encrypted else 0)
        self.send_frame(channel, flags, payload)

    def receive_message(self):
        """Returns (channel, message_id, plaintext_body, flags)."""
        while True:
            while len(self.rx) >= 4:
                flags = self.rx[1]
                header_len = 8 if (flags & 0x03) == FRAME_FIRST else 4
                if len(self.rx) < header_len:
                    break
                length = struct.unpack(">H", self.rx[2:4])[0]
                if len(self.rx) < header_len + length:
                    break

                channel = self.rx[0]
                payload = self.rx[header_len:header_len + length]
                self.rx = self.rx[header_len + length:]

                frame_type = flags & 0x03
                if frame_type == FRAME_FIRST:
                    self.fragments = payload
                    self.fragment_meta = (channel, flags)
                    continue

                if self.fragments:
                    self.fragments += payload
                    if frame_type != FRAME_LAST:
                        continue
                    channel, flags = self.fragment_meta
                    payload = self.fragments
                    self.fragments = b""
                    self.fragment_meta = None

                if flags & FLAG_ENCRYPTED:
                    payload = self.tls.decrypt(payload)

                if len(payload) < 2:
                    continue
                return channel, struct.unpack(">H", payload[:2])[0], payload[2:], flags

            data = self.sock.recv(65536)
            if not data:
                raise EOFError("aa-srcd closed the connection")
            self.rx += data


    def wait_for_timeout(self, channel, message_id, timeout):
        self.sock.settimeout(timeout)
        try:
            return self.wait_for(channel, message_id)
        finally:
            self.sock.settimeout(None)

    def wait_for(self, channel=None, message_id=None):
        """Wait for a specific message, answering pings and parking anything else."""
        for index, queued in enumerate(self.pending):
            if (channel is None or queued[0] == channel) and (message_id is None or queued[1] == message_id):
                return self.pending.pop(index)

        while True:
            received_channel, received_id, body, flags = self.receive_message()

            if received_channel == 0 and received_id == PING_REQUEST:
                self.send_message(0, PING_RESPONSE, body, encrypted=False)
                continue

            if (channel is None or received_channel == channel) and (message_id is None or received_id == message_id):
                return received_channel, received_id, body

            self.pending.append((received_channel, received_id, body))


def build_service_discovery_response():
    video_config = field_varint(1, 1) + field_varint(2, 1) + field_varint(5, 140)   # 800x480, 30fps, 140dpi
    av_channel = field_bytes(4, video_config) + field_varint(1, 3)                  # H264 baseline
    sensor_channel = field_varint(1, 13) + field_varint(1, 10)                      # driving status, night mode
    input_channel = field_varint(1, 3) + field_varint(1, 4)                         # HOME, BACK keycodes

    channels = (
        field_bytes(1, field_varint(1, 3) + field_bytes(3, av_channel))
        + field_bytes(1, field_varint(1, 2) + field_bytes(2, sensor_channel))
        + field_bytes(1, field_varint(1, 1) + field_bytes(4, input_channel))
    )

    return (
        channels
        + field_bytes(2, b"FakeHU")
        + field_bytes(3, b"Test Car")
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5288)
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--ca", required=True)
    parser.add_argument("--frames", type=int, default=20)
    parser.add_argument("--save", help="write the received video stream to this file")
    args = parser.parse_args()

    context = ssl.SSLContext(ssl.PROTOCOL_TLSv1_2)
    context.load_cert_chain(args.cert, args.key)
    context.load_verify_locations(cafile=args.ca)
    context.verify_mode = ssl.CERT_REQUIRED
    context.check_hostname = False

    sock = socket.create_connection((args.host, args.port))
    unit = HeadUnit(sock, TlsLayer(context))

    print("[hu] version request")
    unit.send_message(0, VERSION_REQUEST, struct.pack(">HH", 1, 1))
    _, message_id, body = unit.wait_for(0, VERSION_RESPONSE)
    major, minor, status = struct.unpack(">HHH", body[:6])
    print(f"[hu] source answered v{major}.{minor} status={status}")

    print("[hu] tls handshake")
    out = unit.tls.step()
    while True:
        if out:
            unit.send_message(0, SSL_HANDSHAKE, out)
        _, _, body = unit.wait_for(0, SSL_HANDSHAKE)
        out = unit.tls.step(body)
        if unit.tls.handshake_done and not out:
            break
    print("[hu] tls established")

    print("[hu] auth complete")
    unit.send_message(0, AUTH_COMPLETE, field_varint(1, 0), encrypted=True)

    print("[hu] service discovery response")
    unit.send_message(0, SERVICE_DISCOVERY_RESPONSE, build_service_discovery_response(), encrypted=True)
    _, _, body = unit.wait_for(0, SERVICE_DISCOVERY_REQUEST)
    print(f"[hu] source sent its identity ({len(body)} bytes)")

    print("[hu] opening video channel 3")
    unit.send_message(0, CHANNEL_OPEN_REQUEST, field_varint(1, 2) + field_varint(2, 3), encrypted=True)
    _, _, body = unit.wait_for(0, CHANNEL_OPEN_RESPONSE)
    print(f"[hu] channel open response: {body.hex()}")

    channel, _, body = unit.wait_for(3, AV_SETUP_REQUEST)
    print(f"[hu] AV setup request: codec={body[1] & 0x7F}")
    unit.send_message(3, AV_SETUP_RESPONSE, field_varint(1, 2) + field_varint(2, 8) + field_varint(3, 0),
                      control=False, encrypted=True)

    _, _, body = unit.wait_for(3, AV_START_INDICATION)
    print("[hu] video streaming started")

    # The sensor channel is opened too, so we can check the source answers the
    # sensor start request instead of ignoring the channel.
    unit.send_message(0, CHANNEL_OPEN_REQUEST, field_varint(1, 2) + field_varint(2, 2), encrypted=True)
    _, _, body = unit.wait_for(0, CHANNEL_OPEN_RESPONSE)
    print(f"[hu] sensor channel open response: {body.hex()}")
    # The source asks for each sensor type it wants; we answer once and push data.
    requested = []
    while True:
        try:
            _, _, body = unit.wait_for_timeout(2, SENSOR_START_REQUEST, 1.0)
            requested.append(body[1])
        except TimeoutError:
            break

    print(f"[hu] sensor requests for types: {requested}")
    unit.send_message(2, SENSOR_START_RESPONSE, field_varint(1, 0), control=False, encrypted=True)

    night = field_bytes(10, field_varint(1, 1))                     # NightMode.is_night = true
    driving = field_bytes(13, field_varint(1, 0))                   # DrivingStatus.status = NOT_DRIVING
    unit.send_message(2, SENSOR_EVENT_INDICATION, night + driving, control=False, encrypted=True)
    print("[hu] pushed night mode + driving status events")

    saved = open(args.save, "wb") if args.save else None
    frames = 0
    total_bytes = 0
    pings = 0

    while frames < args.frames:
        channel, message_id, body = unit.wait_for()

        if channel == 0:
            print(f"[hu] unexpected control message 0x{message_id:04x}")
            continue

        # receive_message already removed the 2 byte message id; only the
        # optional 8 byte timestamp is still in front of the picture.
        picture = body
        if message_id == AV_MEDIA_WITH_TIMESTAMP_INDICATION:
            picture = picture[8:]

        if saved:
            saved.write(picture)

        frames += 1
        total_bytes += len(picture)

        if True:  # real head units acknowledge every frame
            ack = field_varint(1, 1) + field_varint(3, frames)
            unit.send_message(3, AV_MEDIA_ACK_INDICATION, ack, control=False, encrypted=True)

    if saved:
        saved.close()

    print(f"[hu] received {frames} video frames, {total_bytes} bytes, answered {pings} pings")
    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
