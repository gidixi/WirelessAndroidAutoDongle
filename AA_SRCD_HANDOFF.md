# aa-srcd — stato, decisioni, note operative

Documento di handoff (denso, pensato per sopravvivere a una compattazione del contesto).
Aggiornato durante il lavoro su `aa-srcd`. Non è documentazione per utenti: è memoria di progetto.

## Obiettivo e scelte già prese

- Mostrare **una nostra dashboard** sul display dell'auto senza telefono: il Raspberry Pi
  impersona la **sorgente Android Auto** (il ruolo del telefono/Gearhead).
- La testata resta quella che è: iniziatrice della sessione e decodificatore video.
- Toolkit UI confermato: **LVGL** (rendering software dentro un buffer ARGB, niente GPU/EGL/DRM).
  Da vendorizzare: Buildroot 2025.02.1 **non** ha un pacchetto `lvgl` (ha qt5/qt6/x264/ffmpeg/wayland).
- Video: prima replay offline di H.264 (`--h264`, fatto), poi render+encode live (`--live`, fatto con
  renderer segnaposto), poi LVGL al posto del segnaposto.
- Niente protobuf/protoc nel target: wire format fatto a mano (`pbwire`) → binario piccolo e nessuna
  eredità GPL-3.0 dai `.proto` di open-android-auto (il repo è MIT). Regola: **non vendorizzare i
  .proto**, riscrivere i campi necessari dalle docs.

## Cosa è scritto e PROVATO (tutto verificato contro una head unit finta, senza auto)

Percorso: `aa_wireless_dongle/package/aa-srcd/`

| Pezzo | Stato |
|---|---|
| Version exchange v1.7 + MATCH | ✅ |
| TLS 1.2 applicativo (server, mutual, BIO in memoria, max proto = TLS1.2) | ✅ certificato peer loggato |
| AUTH_COMPLETE, SERVICE_DISCOVERY (parse canali/risoluzioni/dpi/tipi sensore) | ✅ |
| CHANNEL_OPEN (risponde sia su ch0 sia sul canale target) | ✅ |
| AV setup/start + flusso video con flow control (`max_unacked` + `AVMediaAck`) | ✅ |
| Frammentazione frame (FIRST/MIDDLE/LAST) in uscita e in ingresso | ✅ byte-identici con frame da 1024 e 512 B |
| Ping/pong keepalive **plaintext** anche dopo TLS | ✅ |
| Sensori: `SensorRequest` per tipo annunciato → parse `SensorEventIndication` (night/driving) | ✅ |
| Transport TCP (bench) | ✅ / USB `/dev/usb_accessory`: **scritto, mai eseguito su hardware** |
| Replay H.264 (`H264FileSource`) | ✅ profilo Constrained Baseline, decode pulito |
| Render+encode live (`LiveEncodeSource` + `TestPatternRenderer`) | ✅ 800x480 yuv420p Baseline, contenuto animato |

Ultima prova: `received 15 video frames ... profile=Constrained Baseline ... DECODE OK ...
contenuto animato (frame 0 != frame 12): True`; regressione replay: primi 15029 byte identici.

## Cheat sheet protocollo (quello che ho verificato/sfruttato)

- Framing: `BULK/MIDDLE/LAST` = `[ch][flags][len16be][payload]`; `FIRST` aggiunge `total32be`.
  Flags bit0-1 frame type (00 MIDDLE, 01 FIRST, 10 LAST, 11 BULK), bit2 CONTROL, bit3 ENCRYPTED.
  Header sempre in chiaro; payload cifrato dopo l'handshake; **ping in chiaro**.
- Control channel (id 0): 0x0001 VERSION_REQUEST (raw, non protobuf), 0x0002 VERSION_RESPONSE
  (major/minor/status u16be), 0x0003 SSL_HANDSHAKE, 0x0004 AUTH_COMPLETE, 0x0005 SD_REQUEST (nostro),
  0x0006 SD_RESPONSE (loro), 0x0007 CHANNEL_OPEN_REQUEST, 0x0008 CHANNEL_OPEN_RESPONSE,
  0x000B/0x000C PING (echo timestamp int64 field1), 0x000F/0x0010 SHUTDOWN.
- AV channels (stessi id su tutti gli AV): 0x8000 SETUP_REQUEST (nostro, `media_codec_type`=3 H264_BP),
  0x8003 SETUP_RESPONSE (status OK=2, `max_unacked`, `configs[]`), 0x8001 START_INDICATION (session,config),
  0x8002 STOP_INDICATION, 0x8004 AVMediaAck (`receive_timestamps` field3), 0x8007/0x8008 video focus
  (noi rispondiamo PROJECTED=1). Media data: msg 0x0001 raw Annex-B, oppure 0x0000 + timestamp u64be
  (`--timestamped`). Messaggi AV usano MessageType **SPECIFIC**, non CONTROL.
- Sensori: direzione **HU → source**, ma la richiesta è della source: `SensorRequest{1:type,2:refresh_ms}`
  (msg 0x8001, uno per tipo), risposta `SensorStartResponse{1:status}` (0x8002), eventi
  `SensorEventIndication` (0x8003) con numeri di campo = valori SensorType (10 NIGHT_DATA `{1:bool}`,
  13 DRIVING_STATUS `{1:int32}`).
- Video config: risoluzione enumerata (1=800x480 sempre accettata), fps enum, dpi, margin_width/height.
  Codec: H264_BP=3 (H265/VP9/AV1 gated a GAL ≥ 6.0).
- Channel open: le docs dicono ch0/control, aasdk lo gestisce sul canale target → rispondiamo dove arriva.

## Bug già trovati (non ripeterli)

1. `SSL_set_bio(ssl, internal, network)` sbagliato: con una BIO pair si usa **lo stesso capo** per
   read e write (`internal, internal`) e si scambia dall'altro capo (`network`).
2. Zigzag applicato a `channel_id` (è `int32` plain); lo zigzag è solo di `priority` (sint32).
3. Scartare una "warm-up picture" per verificare la sorgente butta via la prima immagine, che porta
   SPS/PPS → il decoder resta senza parametri per un GOP. Ora c'è `FrameSource::isReady()`.
4. x264 alimentato in BGRA → codifica silenziosamente **High 4:4:4 Predictive** (nessun decoder auto
   lo regge). Serve I420 (conversione interna BT.601) → Baseline garantito.
5. Flow control: se la testata ACKa ogni N frame e noi contiamo 1 ack per messaggio, si va in
   stallo. Tenere il conto dei timestamp ack, e ricordare che senza ack la sorgente si blocca per sempre.
6. Tree dell'repo era tutto CRLF (pericoloso per init script e `aawgd.conf` nel target): normalizzato
   a LF + `.gitattributes` (`* text=auto eol=lf`), `core.autocrlf=false` nel repo locale.

## Riferimenti e licenze

- Corpus protocol (persistente): `W:/Work/Temp/_aa_ref/oaa` (branch `dist`, 248 .proto, **GPL-3.0**),
  `W:/Work/Temp/_aa_ref/oaa-docs` (docs: framing, interactions 01–05, channel/video/sensor),
  `W:/Work/Temp/_aa_ref/aasdk` (riferimento C++ framing/TLS).
- Repo: MIT. Non linkiamo/deriviamo codice GPL; usiamo solo i campi/documentazione.

## Ambiente di lavoro (WSL2 su drvfs) — vincoli reali

- Nessun `sudo` (password richiesta): niente `apt-get`. Docker CLI presente ma **daemon spento**.
- Mount 9p/drvfs su `W:\`: git riporta modifiche fantasma (dev/ino = 0) → `git add -u` le pulisce.
- Strumenti presenti: g++ 11, make, pkg-config, OpenSSL 3.0 dev, ffmpeg/ffprobe, python3, cmake.
  Assenti: protoc, nasm, lvgl.
- x264 static build fatta a mano (effimera, in `/tmp`):
  ```sh
  git clone --depth 1 https://github.com/mirror/x264.git /tmp/x264src
  cd /tmp/x264src && ./configure --prefix=/tmp/x264prefix --enable-static --disable-cli --disable-asm
  make -j$(nproc) && make install     # no nasm → --disable-asm, quindi LENTO: non usarlo come benchmark
  ```
- Build locale del daemon: `PKG_CONFIG_PATH=/tmp/x264prefix/lib/pkgconfig make CXX=g++ PKG_CONFIG=pkg-config`
  (senza x264 trovato, compila comunque e offre solo `--h264`).
- Certificati per il bench (effimeri, in `/tmp/aacert`): `srcd.crt/key` sono quelli embeddati in
  `src/certs.h`; la peer ne usa altri:
  ```sh
  openssl req -x509 -newkey rsa:2048 -keyout hu.key -out hu.crt -days 3650 -nodes -subj "/CN=test head unit"
  ```
- Stream di prova: `ffmpeg -f lavfi -i "color=c=0x1a3a6a:size=800x480:rate=5:duration=6"
  -c:v libx264 -profile:v baseline -level 3.0 -g 15 -pix_fmt yuv420p -an /tmp/dashboard.h264`

## Bench riproducibile (senza auto)

```sh
cd aa_wireless_dongle/package/aa-srcd
(timeout 45 ./aa-srcd --tcp 5304 --live --size 800x480 --fps 5 --bitrate 800 > /tmp/srcd.log 2>&1 &)
python3 -u tools/fake_headunit.py --port 5304 --cert hu.crt --key hu.key --ca srcd.crt \
        --frames 15 --save /tmp/live.h264
ffprobe -show_entries stream=codec_name,profile,width,height,pix_fmt /tmp/live.h264
ffmpeg -v error -i /tmp/live.h264 -f null -      # nessun errore = la testata decodificherebbe
```
`tools/fake_headunit.py` è un peer indipendente (TLS client Python reale): se passa qui, il livello
protocollo è a posto; quello che non copre è il trasporto USB e i capricci specifici della testata.

## M0 alternative: cattura dalla sessione reale (patch già nel repo)

`AAWG_CAPTURE_FRAMES=1` (+ `AAWG_ENABLE_SSH=1`) in `aawgd.conf` → `/var/log/aawgd/capture-*.{bin,idx.csv}`,
analizzabili con `aa_wireless_dongle/package/aawg/tools/capture_report.py`. Serve solo se vorremo
confermare il comportamento della testata reale (canali aperti, dimensioni messaggi, ClientHello,
certificato della testata) partendo da una sessione telefono→auto.

## TODO priorizzati

- **P0 LVGL**: vendorizzare LVGL (pacchetto Buildroot `package/lvgl` o sotto `package/aa-srcd/third_party`)
  + `LvglRenderer : Renderer` (~60 righe: display con draw buffer = ARGB condivisa, flush vuoto,
  `lv_timer_handler()` per frame) + dashboard esempio (orologio, stato veicolo dai sensori, blocchi).
- **P1 Idle/dirty**: ridipingere e codificare solo quando la UI è dirty; IDR periodico (~2 s) per
  tenere il decoder agganciato; target 3–5 fps, 400–800 kbps.
- **P2 Audio silenzioso**: ora rifiutiamo i canali audio; alcune testate protestano → mandare AAC
  silenzioso su media channel (e valutare speech/system).
- **P3 Transport USB su board**: agganciare `UsbAccessoryTransport` al gadget esistente (`S92usb_gadget`,
  uevent `ACCESSORY=START`), avviare `aa-srcd` all'alimentazione, log su syslog.
- **P4 Benchmark encoder sulla board** (x264 con asm, non il build locale senza asm), misurare latenza
  render→schermo.
- **P5 Opzionale encoder HW**: MMAL/VPU legacy solo Pi ≤4; Pi 5 niente H.264 HW. Valutare solo se serve.
- **Commit plan**: (1) `aawgd: frame capture`, (2) `aa-srcd: Android Auto source daemon`,
  (3) `aa-srcd: LVGL dashboard`.

## Stato git

Tre commit locali sopra upstream `ef64522` (working tree pulito, non ancora pushati):
`332852f` normalizzazione LF (+ `.gitattributes`), `29b8fe6` cattura fotogrammi in `aawgd`
(+ `capture_report.py`), `c28e9e8` demone `aa-srcd` (`package/aa-srcd/**`, `package/Config.in`,
`AA_SRCD.md`, questo file).

## Domande ancora aperte (bloccano P3+)

1. Modello testata e se accetta **AA cablato** o solo wireless (se cablato: niente BT/Wi-Fi nel daemon).
2. Board scelta (encoder/asm, margine CPU).
3. Chi avvia Docker per la build Buildroot (io non posso: sudo negato).
