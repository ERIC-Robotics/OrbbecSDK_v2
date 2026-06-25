# lucid_iceoryx

Lucid Vision camera -> iceoryx shared memory -> MCAP pipeline. Discovers Lucid
cameras, publishes BayerRG8 frames over zero-copy shared memory, and writes
timestamped segmented MCAP files readable in Foxglove. Includes optional
GStreamer-based MJPEG capture path and a live health monitor dashboard.

```
lucid_publisher  ->  [iceoryx "Lucid/SN<serial>/Frame"]   ->  lucid_saver   ->  .mcap files
                 ->  [iceoryx "Lucid/SN<serial>/Health"]  ->  lucid_monitor
lucid_saver      ->  [iceoryx "Lucid/SN<serial>/SaverStats"] -> lucid_monitor
```

- **lucid_publisher** — discovers cameras, applies per-camera fps/exposure/gain
  from YAML (or loads a saved UserSet via `--userset`), publishes BayerRG8
  frames over iceoryx. Optional MJPEG HTTP preview of the first camera
  (`--mjpeg`). Publishes per-second health stats. Watchdog detects stalled
  streams and auto-resets them.
- **lucid_saver** — subscribes to frame topics by serial, writes segmented
  `.mcap` files per camera. Publishes live saver stats (fps, latency, segment
  info) to the monitor.
- **lucid_monitor** — live dashboard showing publisher health (fps, exposure,
  gain, loan failures, errors) and saver stats (save fps, write latency,
  segment size) per camera.
- **lucid_mock_publisher** — synthetic BayerRG8 frames, no camera or Arena SDK
  needed. Used to validate the saver and MCAP output in isolation.
- **lucid_viewer** — OpenCV window showing a live iceoryx frame feed.

### GStreamer MJPEG pipeline (optional, Jetson only)

Separate path using hardware JPEG encoding. Does not use iceoryx.

```
lucid_gst_publisher -> shmsink (/tmp/lucid-mjpeg-<serial>.sock) -> lucid_saver_gst -> .mcap
                    -> HTTP MJPEG (base_http_port + camera_index)
```

Built only if GStreamer >= 1.18 is found at configure time.

---

## Dependencies

| Dependency | Version | Notes |
|---|---|---|
| Arena SDK | 0.1.x | Vendor binary from Lucid Vision Labs |
| iceoryx | 2.95.8 | Built from source |
| MCAP | -- | Fetched automatically by CMake |
| fmt, yaml-cpp | system | `apt install` |
| OpenCV | system | core, imgcodecs, imgproc, highgui, videoio |
| GStreamer | >= 1.18 (optional) | Enables `lucid_gst_publisher` / `lucid_saver_gst` |
| CMake | >= 3.16 | |
| Compiler | C++20 | gcc >= 10 |

```bash
sudo apt install -y build-essential cmake git \
    libfmt-dev libyaml-cpp-dev libacl1-dev libncurses-dev \
    libopencv-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    python3-colcon-common-extensions python3-yaml
```

### 1. Install the Arena SDK

Download from [Lucid's downloads hub](https://thinklucid.com/downloads-hub/).

**x86_64:**
```bash
tar -xvzf ArenaSDK_v0.1.x_Linux_x64.tar.gz
cd ArenaSDK_Linux_x64 && sudo sh Arena_SDK_Linux_x64.conf
```

**ARM64 (Jetson AGX Orin):**
```bash
tar -xvzf ArenaSDK_v0.1.x_Linux_ARM64.tar.gz
cd ArenaSDK_Linux_ARM64 && sudo sh Arena_SDK_ARM64.conf
```

### 2. Build and install iceoryx (2.95.8)

```bash
git clone https://github.com/eclipse-iceoryx/iceoryx.git
cd iceoryx && git checkout v2.95.8
cmake -S iceoryx_meta -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build --target install -j$(nproc)
```

---

## Build

```bash
colcon build
```

CMake auto-detects x86_64 vs ARM64 and selects the correct Arena SDK paths.
Default `ARENA_SDK_ROOT`: x86_64 → `/home/eric/ArenaSDK_Linux_x64`,
ARM64 → `/home/eric/ArenaSDK_Linux_ARM64`. Override with:

```bash
colcon build --cmake-args -DARENA_SDK_ROOT=/path/to/ArenaSDK
```
---

## Run (iceoryx pipeline)

**Terminal 1 — RouDi:**
```bash
iox-roudi -c iox_config.toml
```

**Terminal 2 — Publisher:**
```bash
./build/lucid_iceoryx/lucid_publisher
```

Flags:
```
--serial <SN>            capture only this camera (repeatable)
--userset <name>         load a saved camera UserSet (UserSet1/2/3)
--config <path>          override YAML config path
--mjpeg                  enable MJPEG HTTP preview on first camera
--mjpeg-port <N>         MJPEG port (default: 8080)
--mjpeg-quality <0-100>  JPEG quality (default: 85)
```

With `--userset`, fps/exposure/gain/resolution come from the camera's stored
UserSet instead of the YAML values. Use `set_camera_params --userset <name>`
to save a UserSet first.

With `--mjpeg`, point a browser or VLC at `http://<host>:8080/` for a live
preview of the first camera while recording.

**Terminal 3 — Saver:**
```bash
./build/lucid_iceoryx/lucid_saver
```

Flags:
```
--serial <SN>   record only this camera (repeatable, overrides config)
--config <path> override YAML config path
```

Output: `<output_dir>/<serial>/<YYYYMMDD_HHMMSS>_seg001.mcap`, `_seg002.mcap`, ...

**Terminal 4 — Monitor (optional):**
```bash
./build/lucid_iceoryx/lucid_monitor
```

Reads serials from `config/lucid_saver.yaml`. Refreshes every 500ms showing:

```
  SN254400443  ● OK
    pub  fps=89.7  exp=10000us  gain=0.00dB  1440x1080
         published=53820  loan_failures=0  resets=0
    sav  fps=89.5  saved=53810  seg=2  seg_size=847 MiB  write=0.12ms  e2e=3.4ms
```

---

## Validation without a camera

```bash
iox-roudi -c iox_config.toml
./build/lucid_iceoryx/lucid_mock_publisher 254400443 254400442
./build/lucid_iceoryx/lucid_saver
./build/lucid_iceoryx/lucid_monitor   # optional
python3 scripts/check_mcap.py 254400443
```

### Benchmark runs

```bash
python3 scripts/bench_launch.py                                    # mock, 10s
python3 scripts/bench_launch.py --real --serial 254400443 --duration 30
python3 scripts/bench_launch.py --duration 0                       # indefinite
```

---

## Run (GStreamer MJPEG pipeline)

No RouDi needed. Jetson only (`nvjpegenc`).

```bash
./build/lucid_iceoryx/lucid_gst_publisher --serial 254400443
./build/lucid_iceoryx/lucid_saver_gst --serial 254400443
```

HTTP MJPEG preview on `base_http_port + camera_index` (default 9000, 9001, ...).
MCAP uses `foxglove.CompressedVideo` schema — plays directly in Foxglove.

---

## Camera parameter scripts

```bash
# Apply settings + optionally save to a UserSet
./build/lucid_iceoryx/set_camera_params
./build/lucid_iceoryx/set_camera_params --userset UserSet2

# Verify what a UserSet contains
./build/lucid_iceoryx/verify_userset --userset UserSet2
./build/lucid_iceoryx/verify_userset   # current live values
```

---

## Configuration

### `config/lucid_publisher.yaml`

| Key | Default | Description |
|---|---|---|
| `serials` | `[254400443, 254400442]` | Cameras to connect to |
| `width` | 1440 | Requested camera width |
| `height` | 1080 | Requested camera height |
| `fps` | 90 | Target frame rate |
| `exposure_time_us` | 10000 | Exposure in microseconds — must be < `(1/fps)*1e6` |
| `gain_db` | 0.0 | Gain in dB (0 = minimum noise) |
| `image_timeout_ms` | 2000 | `GetImage()` timeout |
| `queue_capacity` | 500 | Internal publisher queue depth |
| `log_dir` | `/home/eric/lucid_iceoryx/logs` | Mission log directory |

These values are ignored when `--userset` is passed.

**Per-camera overrides:**
```yaml
camera_overrides:
  "254400442":
    exposure_time_us: 12000.0
    fps: 60
```

### `config/lucid_saver.yaml`

| Key | Default | Description |
|---|---|---|
| `serials` | `[254400443, 254400442]` | Cameras to record |
| `output_dir` | `data` | Root directory for `.mcap` output |
| `chunk_size_mib` | 128 | MCAP chunk size |
| `segment_size_gib` | 1 | Roll to new file at this size (0 = disable) |

**Expected segment sizes per camera:**

| fps | 1 GiB | 5 GiB | 10 GiB |
|---|---|---|---|
| 30fps | ~11 min | ~55 min | ~110 min |
| 60fps | ~5.5 min | ~27 min | ~55 min |
| 90fps | ~3.7 min | ~18 min | ~37 min |

### `iox_config.toml`

Sized for 1440×1080 BayerRG8 (2 cameras, 16GB RAM):
```toml
size  = 2097152   # 2 MiB per frame
count = 300       # 600 MB total
```

If you change resolution: `size = round_up(40 + W×H×1 + 48, 64)`

Use `common_config.toml` when running the Lucid and MechMind pipelines
together.

---

## Performance metrics

**Publisher** (`PublisherMetrics`) — logged every 2 seconds per camera:
fps, throughput MB/s, software latency avg/min/max, frame jitter, camera drops,
loan failures.

**Saver** (`SubscriberMetrics`) — logged every 2 seconds per camera:
receive fps, write latency avg/min/max, end-to-end latency, transmission drops.

Both are also streamed live to `lucid_monitor` via iceoryx health topics.

---

## iceoryx service description

| Binary | Service | Instance | Event |
|---|---|---|---|
| lucid_publisher | `Lucid` | `SN<serial>` | `Frame` |
| lucid_publisher | `Lucid` | `SN<serial>` | `Health` |
| lucid_saver | `Lucid` | `SN<serial>` | `SaverStats` |
| lucid_saver | subscribes | `SN<serial>` | `Frame` |
| lucid_monitor | subscribes | `SN<serial>` | `Health` + `SaverStats` |

The GStreamer pipeline uses Unix sockets (`/tmp/lucid-mjpeg-<serial>.sock`),
not iceoryx — it runs independently of the above.

---