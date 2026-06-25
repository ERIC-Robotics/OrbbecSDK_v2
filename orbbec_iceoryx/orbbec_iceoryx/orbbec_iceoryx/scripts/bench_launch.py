#!/usr/bin/env python3
import os
import sys
import time
import signal
import subprocess
import argparse
from pathlib import Path
import yaml

# Colorful console output helper
class Colors:
    GREEN = '\033[92m'
    BLUE = '\033[94m'
    YELLOW = '\033[93m'
    RED = '\033[91m'
    BOLD = '\033[1m'
    UNDERLINE = '\033[4m'
    END = '\033[0m'

def log_info(msg):
    print(f"{Colors.BLUE}{Colors.BOLD}[BENCH]{Colors.END} {msg}")

def log_success(msg):
    print(f"{Colors.GREEN}{Colors.BOLD}[BENCH] {msg}{Colors.END}")

def log_warn(msg):
    print(f"{Colors.YELLOW}{Colors.BOLD}[BENCH] {msg}{Colors.END}")

def log_error(msg):
    print(f"{Colors.RED}{Colors.BOLD}[BENCH] {msg}{Colors.END}")

# Global list of running processes to clean up on termination
processes = []

def cleanup_processes(signum=None, frame=None):
    if not processes:
        return
    log_info("Cleaning up running processes...")
    for p in processes:
        if p.poll() is None:
            try:
                # Send SIGINT first for graceful exit
                p.send_signal(signal.SIGINT)
                p.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                p.kill()
            except Exception as e:
                log_warn(f"Failed to kill process: {e}")
    processes.clear()
    
    # Also ensure iox-roudi is stopped
    subprocess.run(["pkill", "-9", "iox-roudi"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["pkill", "-9", "lucid_"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if signum is not None:
        sys.exit(0)

# Register signals for cleanup
signal.signal(signal.SIGINT, cleanup_processes)
signal.signal(signal.SIGTERM, cleanup_processes)

def main():
    parser = argparse.ArgumentParser(description="Lucid Iceoryx Pipeline Benchmark Launcher")
    parser.add_argument("--mock", action="store_true", default=True, help="Use mock publisher (default: True)")
    parser.add_argument("--real", dest="mock", action="store_false", help="Use real camera publisher")
    parser.add_argument("--serial", type=str, default="sncam001", help="Camera serial number (default: sncam001)")
    parser.add_argument("--duration", type=int, default=10, help="Duration of the benchmark in seconds (0 for indefinite, default: 10)")
    parser.add_argument("--fps", type=int, default=90, help="Target FPS for the benchmark (default: 90)")
    parser.add_argument("--width", type=int, default=1440, help="Width of frames (default: 1440)")
    parser.add_argument("--height", type=int, default=1080, help="Height of frames (default: 1080)")
    args = parser.parse_args()

    # Find workspace root dynamically relative to this script's directory
    script_path = Path(__file__).resolve()
    # Path is: <workspace_dir>/lucid_iceoryx/lucid_iceoryx/scripts/bench_launch.py
    workspace_dir = script_path.parents[3]
    package_dir = script_path.parents[1]
    
    # Locate binaries
    bin_dir = workspace_dir / "install" / "lucid_iceoryx" / "bin"
    pub_binary = bin_dir / ("lucid_mock_publisher" if args.mock else "lucid_publisher")
    saver_binary = bin_dir / "lucid_saver"
    
    if not pub_binary.exists() or not saver_binary.exists():
        log_error("Binaries not found. Please compile the workspace first by running 'bash build.sh'")
        sys.exit(1)

    # 1. Terminate any stale processes
    log_info("Cleaning up any existing RouDi or Lucid processes...")
    subprocess.run(["pkill", "-9", "iox-roudi"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["pkill", "-9", "lucid_"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    
    # 2. Setup benchmark specific configurations
    bench_config_dir = workspace_dir / "bench_config"
    bench_config_dir.mkdir(exist_ok=True)
    
    pub_cfg_path = bench_config_dir / "lucid_publisher_bench.yaml"
    sub_cfg_path = bench_config_dir / "lucid_saver_bench.yaml"
    out_data_dir = workspace_dir / "bench_data"
    out_data_dir.mkdir(exist_ok=True)
    
    # Write Publisher Config
    pub_config = {
        "width": args.width,
        "height": args.height,
        "image_timeout_ms": 2000,
        "queue_capacity": 500,
        "fps": args.fps
    }
    with open(pub_cfg_path, "w") as f:
        yaml.dump(pub_config, f)
        
    # Write Subscriber Config
    sub_config = {
        "output_dir": str(out_data_dir),
        "chunk_size_mib": 128,
        "segment_size_gib": 1,
        "serials": [args.serial]
    }
    with open(sub_cfg_path, "w") as f:
        yaml.dump(sub_config, f)
        
    log_info(f"Generated benchmark configuration files under {bench_config_dir.name}/")

    # 3. Start RouDi (Iceoryx daemon)
    log_info("Starting iox-roudi (Iceoryx daemon)...")
    roudi_config_path = package_dir / "iox_config.toml"
    roudi_proc = subprocess.Popen(
        ["iox-roudi", "-c", str(roudi_config_path)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )
    processes.append(roudi_proc)
    time.sleep(1.5)  # Wait for RouDi to initialize shared memory segments
    
    if roudi_proc.poll() is not None:
        log_error("Failed to start iox-roudi daemon. Check permissions or shared memory configuration.")
        sys.exit(1)

    # 4. Start Publisher
    log_info(f"Starting publisher: {pub_binary.name}...")
    pub_cmd = [str(pub_binary), "--config", str(pub_cfg_path)]
    if not args.mock:
        pub_cmd.extend(["--serial", args.serial])
    else:
        # Mock publisher takes serials as positional arguments
        pub_cmd.append(args.serial)
        
    pub_proc = subprocess.Popen(
        pub_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1
    )
    processes.append(pub_proc)

    # 5. Start Saver (Subscriber)
    log_info("Starting subscriber: lucid_saver...")
    sub_cmd = [str(saver_binary), "--config", str(sub_cfg_path)]
    sub_proc = subprocess.Popen(
        sub_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1
    )
    processes.append(sub_proc)

    # Helper function to read output non-blocking
    log_success(f"Benchmark running for {args.duration if args.duration > 0 else 'infinite'} seconds...")
    log_success("Streaming metrics and logs (Ctrl+C to stop early):")
    print("-" * 80)
    
    start_time = time.time()
    
    # Setup non-blocking reads on pipes
    os.set_blocking(pub_proc.stdout.fileno(), False)
    os.set_blocking(sub_proc.stdout.fileno(), False)

    try:
        while True:
            # Check elapsed time
            elapsed = time.time() - start_time
            if args.duration > 0 and elapsed >= args.duration:
                break
                
            # Read publisher output
            try:
                line = pub_proc.stdout.readline()
                if line:
                    line = line.strip()
                    if "[METRICS]" in line or "METRICS" in line:
                        print(f"{Colors.BLUE}[PUB]{Colors.END} {line}")
                    elif "loan failed" in line or "error" in line.lower() or "fail" in line.lower():
                        log_warn(f"[PUB-ERR] {line}")
            except Exception:
                pass

            # Read subscriber output
            try:
                line = sub_proc.stdout.readline()
                if line:
                    line = line.strip()
                    if "[METRICS]" in line or "METRICS" in line:
                        print(f"{Colors.GREEN}[SUB]{Colors.END} {line}")
                    elif "error" in line.lower() or "fail" in line.lower():
                        log_warn(f"[SUB-ERR] {line}")
            except Exception:
                pass

            time.sleep(0.01)
            
    except KeyboardInterrupt:
        log_info("Benchmark interrupted by user.")
        
    print("-" * 80)
    log_info("Stopping benchmark processes...")
    cleanup_processes()
    
    # 6. Analyze resulting MCAP file
    log_info("Searching for recorded MCAP files...")
    mcap_files = list((out_data_dir / args.serial).glob("*.mcap"))
    if not mcap_files:
        log_warn("No MCAP files found in output directory. Check if any frames were received.")
        return

    # Sort to get the most recent one
    mcap_files.sort(key=os.path.getmtime)
    latest_mcap = mcap_files[-1]
    
    log_success(f"Found recorded file: {latest_mcap.name} ({latest_mcap.stat().st_size / (1024 * 1024):.2f} MiB)")
    log_info("Running MCAP check and validation script...")
    print("-" * 80)
    
    # Run check_mcap.py
    check_script = package_dir / "scripts" / "check_mcap.py"
    subprocess.run([sys.executable, str(check_script), str(latest_mcap)])
    print("-" * 80)
    log_success("Benchmark run completed successfully.")

if __name__ == "__main__":
    main()
