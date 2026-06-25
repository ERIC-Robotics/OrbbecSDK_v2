# lucid_iceoryx

Lucid Vision camera → iceoryx shared memory → MCAP pipeline. Discovers Lucid
cameras, publishes BayerRG8 frames over zero-copy shared memory, and writes
timestamped segmented MCAP files readable in Foxglove.

```
lucid_publisher  →  [iceoryx shm "Lucid/SN<serial>/Frame"]  →  lucid_saver  →  .mcap files
                 →  [iceoryx shm "Lucid/SN<serial>/Health"] →  lucid_monitor (live dashboard)
```

- **lucid_publisher** — discovers cameras, spawns one capture worker per camera,
  publishes BayerRG8 frames and per-second health stats over iceoryx. A watchdog
  detects stalled streams and resets them automatically.
- **lucid_saver** — subscribes to camera frame topics by serial, drains frames,
  writes timestamped segmented `.mcap` files per camera. Serials read from config
  by default, overridable via CLI.
- **lucid_monitor** — live health dashboard. Shows fps, exposure, gain, loan
  failures, watchdog resets, and last error per camera. Reads serials from
  `lucid_saver.yaml`.
- **lucid_mock_publisher** — generates synthetic BayerRG8 frames with no camera
  or Arena SDK dependency. Used to validate the saver and MCAP output in isolation.

---

## Dependencies

| Dependency | Version | Notes |
|---|---|---|
| Arena SDK | 0.1.x | Vendor binary from Lucid Vision Labs |
| iceoryx | 2.95.8 | Built from source |
| MCAP | — | Fetched automatically by CMake |
| fmt, yaml-cpp | system | `apt install` |
| CMake | ≥ 3.16 | |
| Compiler | C++20 | gcc ≥ 10 |

```bash
sudo apt install -y build-essential cmake git \
    libfmt-dev libyaml-cpp-dev libacl1-dev libncurses-dev \
    python3-colcon-common-extensions
```

### 1. Install the Arena SDK

Download the archive matching your architecture from
[Lucid's downloads hub](https://thinklucid.com/downloads-hub/) and extract it.

**x86_64:**
```bash
tar -xvzf ArenaSDK_v0.1.x_Linux_x64.tar.gz
cd ArenaSDK_Linux_x64
sudo sh Arena_SDK_Linux_x64.conf
```

**ARM64 (Jetson AGX Orin):**
```bash
tar -xvzf ArenaSDK_v0.1.x_Linux_ARM64.tar.gz
cd ArenaSDK_Linux_ARM64
sudo sh Arena_SDK_ARM64.conf
```

Note the extracted path — passed to CMake as `ARENA_SDK_ROOT`.

### 2. Build and install iceoryx (2.95.8)

```bash
git clone https://github.com/eclipse-iceoryx/iceoryx.git
cd iceoryx && git checkout v2.95.8

cmake -S iceoryx_meta -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build --target install -j$(nproc)
```

Verify: `which iox-roudi`

---

## Build

```bash
colcon build
```

MCAP is fetched automatically. Binaries land in `build/lucid_iceoryx/`.

**ARM64 (Jetson AGX Orin):**
```bash
colcon build --cmake-args -DARENA_SDK_ROOT=/home/eric/ArenaSDK_Linux_ARM64
```

To rebuild a single package after editing:
```bash
colcon build --packages-select lucid_iceoryx
```

---

## Run

**Terminal 1 — RouDi (iceoryx daemon):**
```bash
iox-roudi -c iox_config.toml
```

**Terminal 2 — Publisher:**
```bash
./build/lucid_iceoryx/lucid_publisher
```

Optional flags:
```
--serial <SN>      capture only this camera (repeatable)
--config <path>    override YAML config path
```

By default, serials are read from `config/lucid_publisher.yaml`. If `--serial`
is passed, it overrides the config serials.

**Terminal 3 — Saver:**
```bash
./build/lucid_iceoryx/lucid_saver
```

Optional flags:
```
--serial <SN>      record only this camera (repeatable, overrides config)
--config <path>    override YAML config path
```

Output: `<output_dir>/<serial>/<YYYYMMDD_HHMMSS>_seg001.mcap`, `_seg002.mcap`, ...

**Terminal 4 — Monitor (optional):**
```bash
./build/lucid_iceoryx/lucid_monitor
```

Reads serials from `config/lucid_saver.yaml`. Shows a live dashboard refreshed
every 500ms with per-camera health stats and last error.

---

## Validation without a camera

```bash
# Terminal 1
iox-roudi -c iox_config.toml

# Terminal 2 — mock publisher (no Arena SDK needed)
./build/lucid_iceoryx/lucid_mock_publisher 254400443 254400442

# Terminal 3 — saver (reads serials from config)
./build/lucid_iceoryx/lucid_saver

# Terminal 4 — monitor
./build/lucid_iceoryx/lucid_monitor
```

Verify a recording:
```bash
python3 check_mcap.py 254400443
python3 check_mcap.py 254400442
```

---

## Configuration

### `config/lucid_publisher.yaml`

| Key | Default | Description |
|---|---|---|
| `serials` | `[254400443, 254400442]` | Cameras to connect to |
| `width` | 1440 | Requested camera width (pixels) |
| `height` | 1080 | Requested camera height (pixels) |
| `fps` | 90 | Target frame rate |
| `exposure_time_us` | 10000 | Exposure time in microseconds |
| `gain_db` | 0.0 | Gain in dB (0 = minimum noise) |
| `wb_auto` | false | Auto white balance |
| `wb_red/green/blue` | 1.5/1.0/1.8 | Manual white balance ratios |
| `image_timeout_ms` | 2000 | `GetImage()` timeout |
| `queue_capacity` | 500 | Internal publisher queue depth |
| `log_dir` | `/home/eric/lucid_iceoryx/logs` | Mission log output directory |

**Per-camera overrides** — apply different settings to a specific serial:
```yaml
camera_overrides:
  "254400442":
    exposure_time_us: 12000.0
    fps: 60
```

**Exposure and frame rate constraint:**
```
max_exposure_us = (1 / fps) * 1e6
e.g. at 90fps: max = 11,111us
```
Setting exposure above this silently caps frame rate.

### `config/lucid_saver.yaml`

| Key | Default | Description |
|---|---|---|
| `serials` | `[254400443, 254400442]` | Cameras to record from |
| `output_dir` | `/home/eric/lucid_iceoryx/data` | Root directory for `.mcap` output |
| `chunk_size_mib` | 128 | MCAP chunk size in MiB |
| `segment_size_gib` | 1 | Roll to a new file at this size (0 = disable) |
| `log_dir` | `/home/eric/lucid_iceoryx/logs` | Mission log output directory |

**Expected file sizes per segment per camera:**

| fps | 1 GiB segment | 5 GiB segment | 10 GiB segment |
|---|---|---|---|
| 30fps | ~11 min | ~55 min | ~110 min |
| 60fps | ~5.5 min | ~27 min | ~55 min |
| 90fps | ~3.7 min | ~18 min | ~37 min |

### `iox_config.toml`

Pool sized for 1440×1080 BayerRG8 (1 byte/pixel):

```
frame = kHeaderSize(40) + 1440 × 1080 × 1 = 1,555,240 bytes
chunk = frame + iceoryx overhead (~48 bytes) → round up to 2 MiB
```

If you change resolution, recalculate:
```toml
[[segment.mempool]]
size  = <round_up(40 + W × H × bytes_per_px + 48, 64)>
count = <num_cameras × 256 + headroom>
```

Current (1440×1080 BayerRG8, 2 cameras, 16GB RAM):
```toml
size  = 2097152   # 2 MiB
count = 300       # 300 × 2 MiB = 600 MB
```

---

## Mission logs

Each binary writes a timestamped plain-text log to `log_dir` on every run:

```
logs/lucid_publisher_20260617_110000.log
logs/lucid_saver_20260617_110000.log
logs/lucid_mock_publisher_20260617_110000.log
```

Errors are highlighted in red on the console and written to the log file.

---

## iceoryx service description

| Binary | Service | Instance | Event |
|---|---|---|---|
| lucid_publisher | `Lucid` | `SN<serial>` | `Frame` |
| lucid_publisher | `Lucid` | `SN<serial>` | `Health` |
| lucid_saver | subscribes to Frame | | |
| lucid_monitor | subscribes to Health | | |

Adding a new camera requires only updating the `serials` list in both yaml files
and adjusting `iox_config.toml` pool count. No code changes needed.

---

## ARM64 notes (Jetson AGX Orin)

CMakeLists uses `lib64` for Arena SDK libraries by default (x86_64). On ARM:

```bash
sed -i 's|${ARENA_SDK_ROOT}/lib64|${ARENA_SDK_ROOT}/lib|g' CMakeLists.txt
sed -i 's|Linux64_x64|Linux64_ARM|g' CMakeLists.txt
```