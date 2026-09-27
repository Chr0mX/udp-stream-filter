# Colour — OBS UDP Stream Filter

OBS Studio filter that captures a (optionally cropped / downscaled) region of a
source, JPEG-encodes it on a background thread, and sends it over UDP using a
chunked wire protocol compatible with Axiom’s `udp_receiver.py`.

Filter name in OBS: **UDP Stream (Colour)**

## Features

- Enable/disable streaming without removing the filter
- Target host as IPv4, IPv6, or hostname (Apply button applies address changes
  without recreating the socket on every keystroke)
- Configurable UDP payload size (Wi‑Fi-safe 1200 through LAN 60000)
- JPEG quality + Max FPS cap (default 120)
- Crop presets: 160 / 320 / 416 / 512 / 640, or custom size + anchor
- Optional encode-time downscale after crop
- Green crop overlay on the OBS preview
- Measured FPS / bitrate / drop / error stats (Refresh button; also logged
  every ~5s while streaming)
- Optional XUDP timestamp trailer after JPEG EOI (backward-compatible with
  receivers that only `imdecode` the assembled bytes)

## Install (Windows)

1. Download the installer from a [release](https://github.com/Chr0mX/udp-stream-filter/releases)
   or a CI artifact.
2. Run the installer (installs into
   `%ProgramData%\obs-studio\plugins\Colour\`).
3. Restart OBS Studio.
4. On a source: **Filters → + → UDP Stream (Colour)**.

## Typical setup with Axiom

1. On the receiving machine, start Axiom’s UDP capture on port **5600**.
2. In the filter: set **Target Host** to that machine’s IP/hostname, port
   `5600`, click **Apply Target Address**.
3. Pick a crop preset (or custom crop) matching your detection input size.
4. Enable **Show Crop Overlay** to position the region on the preview.
5. Tick **Enable UDP Streaming**.
6. Use **Refresh Stream Stats** to confirm FPS / bitrate / errors.

## Wire protocol

Each UDP datagram:

| Field | Size | Endian | Meaning |
|---|---|---|---|
| `frame_id` | 4 | BE | Increments per source frame |
| `total_size` | 4 | BE | Total payload bytes across chunks |
| `chunk_index` | 2 | BE | 0-based chunk index |
| `total_chunks` | 2 | BE | Chunks in this frame |
| `chunk_size` | 2 | BE | Payload bytes in this packet |
| payload | `chunk_size` | — | JPEG bytes (+ optional trailer) |

Optional **XUDP** trailer (14 bytes) may follow the JPEG EOI inside the
assembled payload: magic `XUDP`, version `1`, flags `0`, unix-ms timestamp
(uint64 BE). JPEG decoders stop at EOI, so older receivers remain compatible.

## Build

See the [OBS Plugin Template wiki](https://github.com/obsproject/obs-plugintemplate/wiki)
for toolchain setup. This project additionally needs OpenCV (`core`,
`imgproc`, `imgcodecs` with JPEG):

- **Windows CI / local**: vcpkg manifest (`vcpkg.json`), static triplet
  `x64-windows-static`
- **Ubuntu**: `libopencv-dev`
- **macOS**: Homebrew `opencv`

```bash
# Unit tests (no OBS/OpenCV required)
g++ -std=c++17 -o udp_stream_tests tests/test_udp_stream_util.cpp
./udp_stream_tests
```

## Versioning

`buildspec.json` `version` is the single source of truth for the plugin DLL,
archives, and installer. Release tags of the form `1.2.3` overwrite that field
in CI before packaging.
