#!/usr/bin/env python3
import argparse
import csv
import shutil
import subprocess
from pathlib import Path


def read_tsv(path):
    with path.open("r", newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        rows = list(reader)

    required = {"frame_index", "led_on", "offset", "bytes"}
    missing = required - set(reader.fieldnames or [])
    if missing:
        raise ValueError(f"{path} is missing TSV columns: {', '.join(sorted(missing))}")
    return rows


def led_ranges(rows):
    ranges = []
    start = None
    prev = None
    for row in rows:
        index = int(row["frame_index"])
        led_on = int(row["led_on"]) != 0
        if led_on and start is None:
            start = index
            prev = index
        elif led_on:
            prev = index
        elif start is not None:
            ranges.append((start, prev))
            start = None
            prev = None

    if start is not None:
        ranges.append((start, prev))
    return ranges


def drawbox_filter(rows, size):
    terms = []
    for start, end in led_ranges(rows):
        if start == end:
            terms.append(f"eq(n\\,{start})")
        else:
            terms.append(f"between(n\\,{start}\\,{end})")

    if not terms:
        return None

    enable = "+".join(terms)
    return f"drawbox=x=0:y=0:w={size}:h={size}:color=green@0.9:t=fill:enable='{enable}'"


def run_ffmpeg(ffmpeg, input_path, rows, output_path, fps, box_size, rawvideo_args=None):
    filter_text = drawbox_filter(rows, box_size)
    cmd = [ffmpeg, "-y", "-fflags", "+genpts", "-r", str(fps)]

    if rawvideo_args:
        cmd += rawvideo_args

    cmd += ["-i", str(input_path)]

    if filter_text:
        cmd += ["-vf", filter_text]

    cmd += ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(output_path)]
    subprocess.run(cmd, check=True)


def main():
    parser = argparse.ArgumentParser(description="Convert recorded frames to MP4 with LED overlay.")
    parser.add_argument("input_dir", nargs="?", default=".", help="Directory containing frames.tsv and frames.*")
    parser.add_argument("-o", "--output", default="out.mp4", help="Output MP4 path")
    parser.add_argument("--codec", choices=("h264", "h265", "raw"), default=None, help="Recorded stream codec")
    parser.add_argument("--fps", type=float, default=30.0, help="Output frame rate")
    parser.add_argument("--box-size", type=int, default=80, help="LED marker box size in pixels")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="ffmpeg executable")
    args = parser.parse_args()

    if shutil.which(args.ffmpeg) is None and not Path(args.ffmpeg).exists():
        raise SystemExit(f"ffmpeg not found: {args.ffmpeg}")

    input_dir = Path(args.input_dir)
    tsv_path = input_dir / "frames.tsv"
    rows = read_tsv(tsv_path)

    codec = args.codec
    if codec is None:
        if (input_dir / "frames.yuv").exists():
            codec = "raw"
        elif (input_dir / "frames.h265").exists():
            codec = "h265"
        else:
            codec = "h264"

    rawvideo_args = None
    if codec == "raw":
        input_path = input_dir / "frames.yuv"
        first = rows[0] if rows else {}
        missing = {"width", "height", "pix_fmt"} - set(first)
        if missing:
            raise SystemExit(f"raw frames.tsv is missing columns: {', '.join(sorted(missing))}")
        rawvideo_args = [
            "-f",
            "rawvideo",
            "-pix_fmt",
            first["pix_fmt"],
            "-s",
            f"{first['width']}x{first['height']}",
        ]
    else:
        input_path = input_dir / ("frames.h265" if codec == "h265" else "frames.h264")

    if not input_path.exists():
        raise SystemExit(f"missing stream file: {input_path}")

    run_ffmpeg(args.ffmpeg, input_path, rows, Path(args.output), args.fps, args.box_size, rawvideo_args)
    print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
