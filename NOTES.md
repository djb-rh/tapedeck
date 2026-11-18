# TapeDeck development notes

Read this before changing the USB code or the job format.

## The PT-D600 on USB

- 04F9:2074, printer class (7/1/2), bulk OUT 0x02 and IN 0x81, 64-byte packets.
  IEEE 1284 ID: `MFG:Brother;CMD:PT-CBP;MDL:PT-D600;CLS:PRINTER;CID:Brother LabelPrinter TypeA2;`
- Standard Brother P-touch raster protocol: 100 x NUL, `ESC @`, `ESC i a 01`,
  `ESC i z` per page, `ESC i M` (bit 6 auto cut), `ESC i K` (bit 3 = no chain
  printing), `ESC i d` margin dots, `M 02` (PackBits), `G nn nn data` / `Z`
  raster lines, `0x0C` between pages, `0x1A` after the last.
- 128 pins, 180 dpi. Printable pins per tape: 3.5 mm 24 (offset 52), 6 mm 32
  (48), 9 mm 50 (39), 12 mm 70 (29), 18 mm 112 (8), 24 mm 128 (0).
- Status block (`ESC i S`, 32 bytes) model byte 0x6A. The user's 12 mm black-on-white
  TZe reported media type 0x03 "non-laminated", tape colour 0x01, ink 0x08.
- Orientation: MSB of byte 0 of a raster line is the top of the label as read;
  no mirroring.
- During a job the printer sends a status block per phase change and one
  "printing completed" (type 0x01) per page. A label takes ~7 s.
- It flow-controls with NAKs while it prints, so a 512-byte write can block for
  a whole page time (the code allows 30 s).
- ~20 mm leader is cut off at the start of every job (cutter-to-head
  distance). A 79-dot (11 mm) label came out with ~8 mm of trailing tape; the
  D600 probably pads short labels to a minimum length. Not yet measured.

## The MAX3421E driver bug (fixed in lib/)

M5's TinyUSB fork (`hcd_max3421.c`) saved the bulk data toggle only at the end of
a transfer. When the printer NAKs part-way through a multi-packet transfer, the
driver resumes on a later frame with `xact_generic(switch_ep = true)`, which
reloads HCTL from the stale toggle. The next packet then goes out with the
wrong DATA0/1. The printer ACKs it as a retransmission and silently drops it,
64 bytes vanish, and the raster stream misaligns: garbled labels whose position
moves when the data shifts. Fix: save the toggle from HRSL after every NAKed or
successful packet. The SNDFIFO re-arm on OUT NAK retry (USB Host Shield erratum
workaround) is also in, but it alone did not fix the labels. Both are marked
"TapeDeck patch". Any other project that does bulk OUT through this library
needs the same patch.

## Hardware gotchas

- USB Module DIP switches: this unit is SS = CH1 (G13), INT = CH2 (G34). The
  firmware finds CS by reading the REVISION register and INT by resetting the
  chip (OSCOKIRQ pending), enabling only that IRQ and watching which pin
  follows CPUCTL.IE.
- The LCD on this Core comes up at rotation 0 = 320x240 landscape; forcing
  `setRotation(1)` (as UPS Watcher does) made it 240x320 here. `Display::begin`
  keeps M5Unified's default and only rotates if it reports portrait.
  **Unverified: whether the picture is upside down on the real screen.**
- Frame IRQ masking (from UPS Watcher, against the 1 kHz speaker whine) is
  kept: FRAMEIRQ is enabled only while one of our transfers is in flight.

## Job files (TDJ1)

See `include/jobfile.h`. Column-major so the printer task streams a column at a
time from LittleFS (the Core Basic has no PSRAM; nothing holds a whole label in
RAM). The editor appends `SRC1` + length + its design JSON after the pages so
"Edit" in the history can restore the design; the device ignores trailing data.

## The WebServer and raw bodies

Arduino's WebServer never parses the query string of a raw (non-form) POST,
so `/api/print?title=x` loses `title`. Upload options are headers
(`X-Print`, `X-Title`, `X-Copies`, `X-Tape-MM`, `X-Source`) and `/api/save`
stores without printing. ArduinoJson 7 stores `const char*` by pointer:
copy `char[]` fields into `String` when one buffer is reused in a loop
(`/api/jobs` showed one title for every job until that fix).

## Testing without tape or a network

- `x 1` in the probe env, or `tools/devcon.py PORT --boot --job ID out.png`
  in the app, decodes job data to a picture.
- `tools/decode_job.py` decodes a raw raster stream (probe dry run).
- `tools/devcon.py PORT --boot --shot out.png` dumps the Core's screen.
- `httptest` on the console runs a task that calls the device's own web
  server over loopback (all API paths, success and error cases).
- `tools/mock_server.py` serves `web/` with a fake API for working on the
  editor in a browser; it writes uploaded jobs to a directory.

## Status (2026-11-18)

Working on hardware: enumeration, status/tape colours, printing from the
history (device-rendered and browser-made jobs), multi-label batches with
cuts, the Core screen and buttons, captive-portal AP + setup page served, the
HTTP API (via the loopback self-test).

Not yet verified:
- The device on a real Wi-Fi network (mDNS, editor from a phone/laptop, OTA).
- Chain printing ("Hold last label") on the D600.
- The trailing-space minimum on short labels.
- Display orientation on the physical screen.
- D600 auto power-off while idle (the 4 s status poll may keep it awake).
