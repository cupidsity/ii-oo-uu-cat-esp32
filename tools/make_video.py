# turns a video into data/video.bin for the esp32 player.
# usage: python3 tools/make_video.py "/path/to/video.mov"
# then flash it with: tools/flash_video.sh
import struct
import subprocess
import sys
from pathlib import Path

screen_width = 320
screen_height = 170
frames_per_second = 30
# 2 is near the best quality ffmpeg's jpeg encoder gives; frames at this size still average only ~5KB
jpeg_quality = 2

project_folder = Path(__file__).resolve().parent.parent
frames_folder = project_folder / "data" / "frames"
output_path = project_folder / "data" / "video.bin"

if len(sys.argv) > 1:
    source_video = sys.argv[1]
    frames_folder.mkdir(parents=True, exist_ok=True)
    for old_frame in frames_folder.glob("*.jpg"):
        old_frame.unlink()
    # scale to the screen's width, then center-crop the height, so the video fills the screen without stretching
    video_filter = (f"fps={frames_per_second},"
                    f"scale={screen_width}:{screen_height}:force_original_aspect_ratio=increase:flags=lanczos,"
                    f"crop={screen_width}:{screen_height}")
    subprocess.run(["ffmpeg", "-v", "error", "-i", source_video, "-vf", video_filter,
                    "-q:v", str(jpeg_quality), "-pix_fmt", "yuvj420p",
                    str(frames_folder / "frame_%04d.jpg")], check=True)

frame_paths = sorted(frames_folder.glob("frame_*.jpg"))
if not frame_paths:
    sys.exit("no frames found; pass a video file path")

# layout must match VideoHeader in src/main.cpp: magic, frame count, fps, width, height,
# then one (offset, length) pair per frame, then the jpeg bytes back to back
magic = b"VID1"
header_size = 4 + 4 * 4
index_size = len(frame_paths) * 8
frame_bytes = [path.read_bytes() for path in frame_paths]

index_entries = bytearray()
next_offset = header_size + index_size
for jpeg in frame_bytes:
    index_entries += struct.pack("<II", next_offset, len(jpeg))
    next_offset += len(jpeg)

with open(output_path, "wb") as output_file:
    output_file.write(magic)
    output_file.write(struct.pack("<IIII", len(frame_paths), frames_per_second, screen_width, screen_height))
    output_file.write(index_entries)
    for jpeg in frame_bytes:
        output_file.write(jpeg)

print(f"wrote {output_path}: {len(frame_paths)} frames, {next_offset / 1024 / 1024:.2f} MB, "
      f"{len(frame_paths) / frames_per_second:.1f} seconds")
