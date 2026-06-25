import struct
from mcap.reader import make_reader
import sys

if len(sys.argv) < 2:
    print("Usage: python3 check_mcap.py <path_to_mcap>")
    sys.exit(1)

path = sys.argv[1]
print(f"Reading: {path}\n")

frames = []
with open(path, "rb") as f:
    reader = make_reader(f)
    for schema, channel, message in reader.iter_messages():
        ts, seq, w, h, pf, pad, data_size = struct.unpack_from("<QQIIIIQ", message.data, 0)
        frames.append((ts, seq))

print(f"Total frames: {len(frames)}")
print(f"First seq: {frames[0][1]}  Last seq: {frames[-1][1]}")
print()

# Calculate fps per second using camera hardware timestamps
buckets = {}
for ts_ns, seq in frames:
    second = ts_ns // 1_000_000_000
    buckets.setdefault(second, []).append(seq)

print("Frames per second (camera hardware timestamps):")
for second in sorted(buckets.keys()):
    seqs = buckets[second]
    print(f"  t={second}s  frames={len(seqs)}  seq={seqs[0]}..{seqs[-1]}")

print()

# Check for sequence gaps
gaps = 0
for i in range(1, len(frames)):
    if frames[i][1] != frames[i-1][1] + 1:
        gaps += 1
        print(f"Gap: seq {frames[i-1][1]} → {frames[i][1]}")
print(f"Total sequence gaps in saved file: {gaps}")

# Inter-frame interval stats
if len(frames) > 1:
    intervals = [(frames[i][0] - frames[i-1][0]) / 1e6
                 for i in range(1, len(frames))]
    avg_ms = sum(intervals) / len(intervals)
    print(f"\nAvg inter-frame interval: {avg_ms:.2f} ms  (~{1000/avg_ms:.1f} fps)")
    print(f"Min: {min(intervals):.2f} ms  Max: {max(intervals):.2f} ms")