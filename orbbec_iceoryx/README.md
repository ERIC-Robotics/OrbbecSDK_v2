# orbbec_iceoryx

![alt text](docs/system-diagram.png)

V4L2 camera → iceoryx shared memory → MCAP pipeline. Captures from any V4L2/UVC
camera (raw YUYV/NV12 or passthrough MJPEG), publishes frames over zero-copy
shared memory, and writes timestamped segmented MCAP files readable in
Foxglove. Includes a GStreamer-based hardware MJPEG path and a live health
monitor dashboard.

```
orbbec_publisher  →  [iceoryx "Orbbec/<name>/Frame"]   →  orbbec_saver   →  .mcap files
                  →  [iceoryx "Orbbec/<name>/Health"]  →  orbbec_monitor
orbbec_saver      →  [iceoryx "Orbbec/<name>/SaverStats"] → orbbec_monitor
```

- **orbbec_publisher** — opens V4L2 devices (by path or by USB serial),
  publishes frames over iceoryx. If the camera outputs MJPEG natively the
  compressed frames are forwarded as-is on the `MJPEG` topic (no
  re-encoding); raw formats (YUYV, NV12) go out on the `Frame` topic.
  Publishes per-second health stats. `--list-formats` probes attached
  cameras without needing RouDi running.
- **orbbec_saver** — subscribes to `Frame` topics by device name, writes
  segmented `.mcap` files per camera. Publishes live saver stats (fps,
  latency, segment info) to the monitor.
- **orbbec_monitor** — live dashboard showing publisher health (fps,
  resolution, loan failures, errors) and saver stats (save fps, write
  latency, segment size) per camera.
- **orbbec_mock_publisher** — generates synthetic frames without a camera.
  Used for benchmarking and validating the saver/MCAP pipeline in isolation.
  Optionally integrated into `bench_launch.py` for automated end-to-end runs.
- **orbbec_viewer** — OpenCV window showing live frames from iceoryx or HTTP.
  Modes: raw frames (default), iceoryx MJPEG (`--mjpeg`), or HTTP MJPEG stream
  (`--mjpeg-stream` for remote viewing via `orbbec_gst_publisher --stream`).

### GStreamer MJPEG pipeline (optional)

Separate path using hardware JPEG encoding where available.

```
orbbec_gst_publisher → [iceoryx "Orbbec/<name>/MJPEG"] → orbbec_saver_gst → .mcap
                     → HTTP MJPEG server (--stream)
```

Encoder backend is auto-detected (override with `--jetson` / `--cpu`):
Jetson `nvvidconv`+`nvjpegenc` → desktop NVIDIA `nvjpegenc` → software
`jpegenc`. If the camera already outputs MJPEG, frames pass through
undecoded. `orbbec_saver_gst` writes MCAP using the `foxglove.CompressedImage`
schema. Built only if GStreamer ≥ 1.18 is found at configure time.

`--raw-stream <socket>` additionally exposes raw NV12/RGB frames over a
`nvunixfdsink` Unix socket (Jetson) for live preview with `scripts/raw_viewer.py`,
without touching the MCAP/MJPEG path.

---

## Dependencies

| Dependency | Version | Notes |
|---|---|---|
| iceoryx | 2.95.8 | Built from source |
| MCAP | -- | Fetched automatically by CMake (`FetchContent`) |
| fmt, yaml-cpp | system | `apt install` |
| OpenCV | system | core, imgcodecs, imgproc, highgui, videoio |
| GStreamer | >= 1.18 (optional) | Enables `orbbec_gst_publisher` / `orbbec_saver_gst` |
| CMake | >= 3.16 | |
| Compiler | C++20 | gcc >= 10 |

```bash
sudo apt install -y build-essential cmake git \
    libfmt-dev libyaml-cpp-dev libacl1-dev libncurses-dev \
    libopencv-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    python3-yaml
```

No vendor camera SDK is required — capture goes through the standard V4L2
kernel API, so any UVC-compliant camera works.

### Build and install iceoryx (2.95.8)

```bash
git clone https://github.com/eclipse-iceoryx/iceoryx.git
cd iceoryx && git checkout v2.95.8
cmake -S iceoryx_meta -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build --target install -j$(nproc)
```

---

## Build
```bash
cmake -B build -S orbbec_iceoryx/orbbec_iceoryx -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build
```

Binaries land in `build/`. Config YAML files are copied next to them at
build time from `orbbec_iceoryx/orbbec_iceoryx/config/`.

---

## Run (iceoryx pipeline)

**Terminal 1 — RouDi:**
```bash
iox-roudi -c orbbec_iceoryx/orbbec_iceoryx/iox_config.toml
```

**Terminal 2 — Publisher:**
```bash
./build/orbbec_publisher
```

Flags:
```
--serial <s1,s2,...>     publish cameras matching these USB serial numbers
--device <path> <name>   capture from this V4L2 device (repeatable)
--format <fmt>           pixel format: MJPEG, YUYV, NV12 (default: MJPEG)
--width  <px>            frame width  (default: from config)
--height <px>            frame height (default: from config)
--list-formats           print supported formats for each device and exit
--config <path>          YAML config (default: config/orbbec_publisher.yaml)
```

With `--serial`, the iceoryx instance name is the serial number itself. Use
`--list-formats` to discover serials/formats for attached cameras (no RouDi
needed for this).

**Terminal 3 — Saver:**
```bash
./build/orbbec_saver
```

Flags:
```
--name <name>   record only this device (repeatable, overrides config)
--config <path> override YAML config path
```

Output: `<output_dir>/<name>/<YYYYMMDD_HHMMSS>_seg001.mcap`, `_seg002.mcap`, ...

**Terminal 4 — Monitor (optional):**
```bash
./build/orbbec_monitor
```

Reads device names from `config/orbbec_saver.yaml`. Refreshes every 500ms
showing publisher fps/resolution/loan failures and saver fps/write
latency/segment size per camera.

**Terminal 5 — Viewer (optional):**
```bash
./build/orbbec_viewer --name cam0
```

Flags:
```
--name <name>       camera device name (default: cam0)
--mjpeg             subscribe to iceoryx MJPEG topic instead of raw frames
--mjpeg-stream      connect to HTTP MJPEG server (for remote viewing)
--mjpeg-url <url>   HTTP stream URL (default: http://localhost:9000/)
```

Three viewing modes:
- **Raw frames** (default) — zero-copy iceoryx `Frame` subscription, YUYV→BGR conversion in OpenCV
- **MJPEG over iceoryx** (`--mjpeg`) — bandwidth-efficient, subscribes to `MJPEG` topic
- **HTTP MJPEG** (`--mjpeg-stream`) — remote viewing over network, requires `orbbec_gst_publisher --stream`

---

## Validation without a camera

**Manual validation** — start each component separately:

```bash
iox-roudi -c orbbec_iceoryx/orbbec_iceoryx/iox_config.toml
./build/orbbec_mock_publisher cam0 cam1
./build/orbbec_saver
./build/orbbec_monitor   # optional
python3 orbbec_iceoryx/orbbec_iceoryx/scripts/check_mcap.py data/cam0/<file>.mcap
```

**Automated benchmark** — launch, run, and validate in one script:

```bash
python3 orbbec_iceoryx/orbbec_iceoryx/scripts/bench_launch.py
```

Flags:
```
--mock              use synthetic publisher (default: True)
--real              use real camera publisher
--serial <name>     camera serial or mock name (default: sncam001)
--duration <s>      run time in seconds; 0 = indefinite (default: 10)
--fps <n>           target frame rate (default: 90)
--width <px>        frame width (default: 1440)
--height <px>       frame height (default: 1080)
```

The script automates RouDi startup, spawns publisher and saver, streams live metrics (fps, latency, errors), and validates the recorded MCAP on completion.

---

## Run (GStreamer MJPEG pipeline)

```bash
./build/orbbec_gst_publisher --device /dev/video0 --name cam0 --stream
./build/orbbec_saver_gst --name cam0
```

Flags (`orbbec_gst_publisher`):
```
--device      <path>   V4L2 device (default: interactive picker)
--name        <str>    stream name / iceoryx instance
--format      <fmt>    MJPEG for passthrough, omit for encode
--width       <px>     capture width  (default: 1280)
--height      <px>     capture height (default: 720)
--fps         <n>      frame rate (default: 30)
--quality     <1-100>  JPEG quality for encoding (default: 85)
--port        <n>      HTTP MJPEG server port (default: 9000)
--jetson               force Jetson nvvidconv+nvjpegenc
--cpu                  force jpegenc (software)
--stream               enable HTTP MJPEG server
--raw-stream  <path>   enable nvunixfdsink raw NV12/RGB branch (Jetson)
```

With `--stream`, point a browser or VLC at `http://<host>:<port>/` for a live
preview while recording.

Flags (`orbbec_saver_gst`):
```
--name <name>    device to record (repeatable)
--config <path>  YAML config
--out <dir>      override output directory
```

MCAP uses the `foxglove.CompressedImage` schema — plays directly in Foxglove.

---

## Configuration

### `config/orbbec_publisher.yaml`

| Key | Default | Description |
|---|---|---|
| `devices` | `[{path: /dev/video0, name: cam0}, {path: /dev/video2, name: cam1}]` | V4L2 devices to capture from |
| `width` | 1280 | Requested camera width |
| `height` | 720 | Requested camera height |
| `fps` | 30 | Target frame rate |
| `pixel_format` | YUYV | `YUYV` (packed 4:2:2) or `MJPEG` (hardware-compressed) |
| `image_timeout_ms` | 2000 | Frame read timeout |
| `queue_capacity` | 500 | Internal publisher queue depth |

The device `name` is used as the iceoryx instance identifier and can be
overridden per-device with `--device <path> <name>`.

### `config/orbbec_saver.yaml`

| Key | Default | Description |
|---|---|---|
| `devices` | `["cam0", "cam1"]` | Device names to subscribe to (must match publisher names) |
| `output_dir` | `data` | Root directory for `.mcap` output |
| `chunk_size_mib` | 128 | MCAP chunk size |
| `segment_size_gib` | 1 | Roll to new file at this size (0 = disable) |

### `config/orbbec_gst_publisher.yaml`

| Key | Default | Description |
|---|---|---|
| `device` | `/dev/video0` | V4L2 device path |
| `name` | `cam0` | Stream name — iceoryx instance `Orbbec/<name>/MJPEG` |
| `width` / `height` / `fps` | 1280 / 720 / 30 | Capture resolution and rate |
| `quality` | 85 | MJPEG encoder quality (1-100) |
| `http_port` | 9000 | HTTP MJPEG server port |

### `iox_config.toml`

Sized for RGB8/YUYV frames up to ~2448×2048 (2 MiB pool, count 300 ≈ 600 MB).
See the comments in the file for the sizing formula if resolution changes.
`common_config.toml` is an alternate RouDi profile for running alongside
other iceoryx publishers (larger combined pool) — pass it with `iox-roudi -c`
if it fits your setup.

---

## Performance metrics

**Publisher** — logged every 2 seconds per camera: fps, resolution, loan
failures, watchdog resets, errors. Also streamed live to `orbbec_monitor` via
the `Health` topic (`orbbec::HealthMsg`).

**Saver** — logged every 2 seconds per camera: save fps, write latency,
end-to-end latency, throughput, transmission drops, current segment.
Streamed live via the `SaverStats` topic (`orbbec::SaverStatsMsg`).

---

## iceoryx service description

| Binary | Service | Instance | Event |
|---|---|---|---|
| orbbec_publisher | `Orbbec` | `<name>` | `Frame` (raw) |
| orbbec_publisher | `Orbbec` | `<name>` | `MJPEG` (passthrough) |
| orbbec_publisher | `Orbbec` | `<name>` | `Health` |
| orbbec_saver | subscribes | `<name>` | `Frame` |
| orbbec_saver | `Orbbec` | `<name>` | `SaverStats` |
| orbbec_gst_publisher | `Orbbec` | `<name>` | `MJPEG` |
| orbbec_saver_gst | subscribes | `<name>` | `MJPEG` |
| orbbec_monitor | subscribes | `<name>` | `Health` + `SaverStats` |

`<name>` is either the device name from config/`--device`, or the USB serial
number when `--serial` is used.

---

## Repo layout

```
orbbec_iceoryx/
├── common_config.toml                       # alternate RouDi pool profile
├── laptop_config.toml                       # reduced-footprint RouDi profile
└── orbbec_iceoryx/                          # CMake project
    ├── CMakeLists.txt
    ├── cmake/dependencies.cmake             # FetchContent for MCAP
    ├── config/                              # per-binary YAML defaults
    ├── include/                             # shared headers (iceoryx message structs, loggers, mcap writers, v4l2 probe)
    ├── src/                                 # publisher/saver/monitor/gst binaries
    ├── iox_config.toml                      # default RouDi shared memory config
    └── scripts/
        ├── bench_launch.py                  # automated end-to-end benchmark launcher
        ├── check_mcap.py                    # MCAP validation and frame statistics
        ├── raw_viewer.py                    # live frame preview over Unix socket (Jetson)
        └── orbbec_viewer                    # OpenCV window showing live iceoryx feed
```
