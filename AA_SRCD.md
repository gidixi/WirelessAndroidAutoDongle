# aa-srcd: showing our own app on the head unit

> Memoria di progetto (stato, decisioni, cheat sheet protocollo, comandi di bench): `AA_SRCD_HANDOFF.md`.

Goal: the Raspberry Pi becomes the **Android Auto source** and projects its own
content (first target: a static dashboard) on a car head unit, with no phone in
the loop.

Today `aawgd` is a pipe: the phone runs the whole Android Auto stack and the Pi
forwards its TCP stream to `/dev/usb_accessory`. To project our own content we
must implement what the phone implements. That daemon is called `aa-srcd` here.

## Who plays which role

Everything documented in the Android Auto protocol assumes the head unit
initiates. When `aa-srcd` connects to a car, the car behaves exactly like a
normal head unit and the Pi has to behave like a phone (`Gearhead`):

| Phase | Car head unit does | `aa-srcd` must do |
|-------|--------------------|-------------------|
| USB | host, sends AOA strings, then `ACCESSORY=START` uevent | enumerate as accessory device (`VID 0x18D1`, `PID 0x2D00`) via `f_accessory` gadget — already done by this repo |
| Version | sends `VERSION_REQUEST` (0x0001), raw binary | reply `VERSION_RESPONSE` (0x0002) with major/minor + `MATCH` before anything else |
| TLS | is the **TLS client**, wraps handshake bytes in `SSL_HANDSHAKE` (0x0003) control messages | be the **TLS server**: TLS 1.2, self-signed cert, mutual auth; wrap our bytes in the same message type |
| Auth | sends `AUTH_COMPLETE` (0x0004) | accept (optionally the extended Binding exchange) |
| Discovery | sends `SERVICE_DISCOVERY_RESPONSE` (0x0006) listing channels/resolutions/sensors | send `SERVICE_DISCOVERY_REQUEST` (0x0005) with device name/brand/icons |
| Channels | `CHANNEL_OPEN_REQUEST` (0x0007) per service | `CHANNEL_OPEN_RESPONSE` (0x0008); channel ids come from the discovery response, never hardcoded |
| Video | `SETUP_REQUEST` (0x8000) then start request on the AV channel | `SETUP_RESPONSE` (0x8003) status READY, `START_INDICATION` (0x8001), then push H.264 NAL units as `AV_MEDIA_INDICATION` (0x0001) |
| Input | sends touch/key events | optional for a static dashboard; at least tolerate them |
| Sensors | advertises sensor types; expects periodic data | send `DRIVING_STATUS` and `NIGHT_DATA` at minimum |
| Keepalive | `PING_REQUEST` (0x000b) | `PING_RESPONSE` (0x000c), always **plaintext** even after TLS |

## Wire format essentials

```
BULK/MIDDLE/LAST : [channel:1][flags:1][payload_len:2 BE][payload]
FIRST (fragments): [channel:1][flags:1][payload_len:2 BE][total_msg_size:4 BE][payload]
payload starts with [message_id:2 BE], scoped per channel
```

Flags low nibble: bits 0-1 frame type (`00` MIDDLE, `01` FIRST, `10` LAST, `11`
BULK), bit 2 control message, bit 3 payload encrypted. Frame headers stay
plaintext; payloads are encrypted after the TLS handshake; pings stay plaintext.

## Minimum configuration for a static dashboard

* Video: H.264 baseline profile, one resolution (800x480 is always accepted),
  low fps (1-5 fps is plenty for a static image, but keep periodic keyframes so
  the head unit can join at any time and answer its `I-frame` demands).
* Audio: skip the audio channels entirely if silent (degraded, still projects).
* Input: advertise nothing, or advertise keycodes `HOME/BACK/SEARCH` and ignore.
* Sensors: `DRIVING_STATUS` + `NIGHT_DATA`.
* No Bluetooth/Wi-Fi needed **if** the head unit accepts wired AA. If it only
  does wireless AA, keep the existing BT pairing + BLE advertisement + RFCOMM
  Wi-Fi credential exchange and terminate the TCP session instead of proxying it.

## Suggested module layout

```
src/transport/   AOA gadget (/dev/usb_accessory) or TCP socket, byte stream only
src/messenger/   frame reader/writer, fragmentation, encrypt/decrypt (OpenSSL BIO pair)
src/control/     version, TLS, auth, discovery, ping, shutdown state machine
src/av/          video channel (H.264 frames out), audio channels (optional)
src/sensor/      driving status + night mode
src/content/     the dashboard: render once to RGBA, encode with x264/v4l2m2m,
                 re-emit keyframe on demand
```

Reuse from this repository: the `f_accessory` kernel patches, the USB gadget
setup (`S92usb_gadget`, `usb.cpp`), the uevent monitor, the config/logging
helpers, and the Buildroot packaging.

## Milestones

| # | Deliverable | Acceptance | Stop condition |
|---|-------------|-----------|----------------|
| M0 | Frame capture in `aawgd` (done, see below) | one real phone→car session produces both capture files | stop after 2 good sessions |
| M1 | Protocol trace analysis of that capture | list of channels the car opens, sizes, timings, TLS params, certificate | stop when the sequence is known |
| M2 | Bench head unit (`aa-srcd/tools/fake_headunit.py`) driving `aa-srcd` end to end — **done** | version v1.7, mutual TLS 1.2, auth, discovery, channel open, AV setup/start, 60 H.264 pictures received byte-identical through 1 KB fragmented frames, ping/pong answered | — |
| M3 | Version/TLS/discovery against the real car | car shows "connecting", no error, session survives 60 s | stop when stable, or when we know which message the car rejects |
| M4 | Video channel: static test pattern reaches the car screen | dashboard visible on the head unit | stop when pixels appear |
| M5 | Robustness: unplug/replug, ignition cycles, I-frame requests, timeouts | reconnects automatically like `aawgd` does today | stop after 20 unattended cycles |
| M6 | The actual dashboard app, rendered with **LVGL** (vendored: Buildroot 2025.02.1 has no `lvgl` package) behind `LvglRenderer : Renderer` | content readable while driving | product decision, not protocol work |

Do not start M4 before M3 is green: without a stable session, video failures are
impossible to attribute.

The render side is already wired independently of the car: `--live` renders into a
32-bit ARGB buffer, converts to I420 and encodes with x264 (`zerolatency`, Baseline,
normalized 800x480), which the fake head unit decodes cleanly. Swapping the test
pattern for LVGL does not touch the protocol code.

## M0: capturing your head unit

The payloads between phone and head unit are encrypted end to end, so the Pi
cannot read the messages — but frame headers, sizes, ordering and everything
before the TLS handshake (version exchange, ClientHello, the head unit's own
certificate) are in clear. That is enough to learn how a specific car behaves.

Enable it in `aawgd.conf` on the `WirelessAA` drive:

```sh
AAWG_CAPTURE_FRAMES=1
AAWG_ENABLE_SSH=1
```

Connect the phone once, then copy the files off the dongle:

```sh
scp -r root@10.0.0.1:/var/log/aawgd .
python3 aa_wireless_dongle/package/aawg/tools/capture_report.py \
    aawgd/capture-from-phone.idx.csv aawgd/capture-from-headunit.idx.csv
```

`capture-*.bin` holds the exact bytes of each direction; `capture-*.idx.csv` has
one line per frame (time, channel, flags, sizes, offsets into the `.bin`). The
extracted certificate and the pre-TLS frames can be pulled out of the `.bin`
with the offsets from the index.

## Reference material and licensing

Protocol definitions and reverse-engineered specs live in
[`mrmees/open-android-auto`](https://github.com/mrmees/open-android-auto)
(**GPL-3.0**), framing/crypto reference code in
[`f1xpl/aasdk`](https://github.com/f1xpl/aasdk). This repository is MIT. Keep the
two separated: use the references to write our own message structs rather than
vendoring their generated protobuf sources, or ship `aa-srcd` as its own package
with the matching license notice.

## The daemon today

`package/aa-srcd/` holds a working source implementation, deliberately without
protoc/libprotobuf: `pbwire.cpp` encodes and decodes just the protobuf fields we
need, so the binary stays small and we do not inherit the GPL-3.0 proto files.

```
src/pbwire.{h,cpp}      minimal protobuf wire reader/writer (skips unknown fields)
src/messenger.{h,cpp}   frame reader/writer, fragmentation, app-layer TLS 1.2 server
src/transport.{h,cpp}   /dev/usb_accessory (production) or TCP listener (bench)
src/certs.h             embedded self-signed identity (subject avoids the two rejected names)
src/srcd.cpp            session state machine + H.264 file replay with flow control
tools/fake_headunit.py  independent head unit used as the test peer
```

Bench it on any Linux box (needs OpenSSL dev headers and ffmpeg only for the test stream):

```sh
cd aa_wireless_dongle/package/aa-srcd
make CXX=g++ PKG_CONFIG=pkg-config
# the fake head unit needs its own certificate; srcd.crt can be extracted from src/certs.h
openssl req -x509 -newkey rsa:2048 -keyout hu.key -out hu.crt -days 3650 -nodes \
        -subj "/CN=btest head unit"
ffmpeg -f lavfi -i "color=c=navy:size=800x480:rate=5:duration=6" \
       -c:v libx264 -profile:v baseline -level 3.0 -g 15 -pix_fmt yuv420p -an /tmp/dashboard.h264
./aa-srcd --tcp 5288 --h264 /tmp/dashboard.h264 --fps 5 --debug &
python3 tools/fake_headunit.py --port 5288 --cert hu.crt --key hu.key --ca srcd.crt \
       --frames 60 --save /tmp/received.h264
ffprobe /tmp/received.h264          # should report h264 800x480
```

Useful switches: `--max-frame <n>` forces frame fragmentation (the path most likely
to expose bugs), `--timestamped` sends the timestamped media indication instead of
the plain one, `AA_SRCD_DEBUG_TLS=1` logs TLS record/state details.

What is still missing before a real car works: the USB accessory path has never
run on hardware (only the TCP transport was exercised), audio channels are
refused rather than streamed, and we assume the head unit opens channels on the
control channel — if a car opens them on the target channel instead, the code
already answers there too, but this needs confirming on real hardware. Sensors
flow head unit -> source: we send a `SensorRequest` per advertised type
(`NIGHT_DATA`, `DRIVING_STATUS`) and parse the pushed `SensorEventIndication`
(verified against the bench peer), but nothing acts on night mode yet.

## Open questions

1. Does the target head unit accept **wired** AA, or wireless only? Wired drops
   the whole Bluetooth/Wi-Fi branch and halves the work.
2. Which board? Pi 4/5 have a hardware H.264 encoder; Zero W / Zero 2 W do not
   (fine for a static dashboard, changes the pipeline).
3. Latency budget for refreshes (clock, camera feed, ...).
