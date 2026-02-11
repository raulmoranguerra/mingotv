import os
import re
import subprocess
import platform
from typing import Optional

directory = os.path.dirname(os.path.realpath(__file__))
out_dir = os.path.join(directory, "encoded")
os.makedirs(out_dir, exist_ok=True)

VIDEO_EXTS = {".mp4", ".mkv", ".mov", ".avi"}

def is_video(f: str) -> bool:
    return os.path.splitext(f.lower())[1] in VIDEO_EXTS

def slugify(text: str) -> str:
    # seguro para Windows/macOS/Linux
    t = text.lower()
    t = re.sub(r"[^\w\s-]", "", t)          # quita puntuación rara
    t = re.sub(r"[\s-]+", "_", t).strip("_")
    return t

def extract_episode_stem(filename: str) -> Optional[str]:
    """
    Devuelve el nombre base SIN extensión para el output:
      1) sXXeYY -> "s01e01"
      2) "Serie 005 - ..." / "Serie_047_- ..." -> "serie_e005" / "serie_e047"
    """
    name = os.path.splitext(filename)[0]
    low = name.lower()

    # 1) sXXeYY
    m = re.search(r"s(\d{2})e(\d{2})", low)
    if m:
        return f"s{m.group(1)}e{m.group(2)}"

    # 2) "Serie 005 ..." / "Serie_047 ..."
    m = re.match(r"^\s*(.+?)[\s_]+(\d{1,4})\b", name)
    if m:
        series = slugify(m.group(1))
        ep_num = int(m.group(2))
        ep = f"{ep_num:03d}" if ep_num < 1000 else str(ep_num)
        return f"{series}_e{ep}"

    return None

def get_src_fps(path: str) -> float:
    r = subprocess.run(
        [
            "ffprobe", "-v", "error",
            "-select_streams", "v:0",
            "-show_entries", "stream=avg_frame_rate",
            "-of", "default=nk=1:nw=1",
            path
        ],
        capture_output=True, text=True
    )
    s = (r.stdout or "").strip()
    if not s or s == "0/0":
        return 0.0
    if "/" in s:
        n, d = s.split("/", 1)
        try:
            return float(n) / float(d)
        except Exception:
            return 0.0
    try:
        return float(s)
    except Exception:
        return 0.0

def pick_fps_mode(src_fps: float):
    fps_23976 = 24000 / 1001
    if src_fps <= 0:
        return ("25", 50, 50, "fallback->25")
    if abs(src_fps - 25.0) <= abs(src_fps - fps_23976):
        return ("25", 50, 50, f"{src_fps:.3f}->25")
    else:
        return ("24000/1001", 48, 48, f"{src_fps:.3f}->23.976")

def ffmpeg_has_encoder(encoder_name: str) -> bool:
    r = subprocess.run(["ffmpeg", "-hide_banner", "-encoders"], capture_output=True, text=True)
    return encoder_name in (r.stdout or "")

def cuda_available() -> bool:
    if not ffmpeg_has_encoder("h264_nvenc"):
        return False
    r = subprocess.run(["nvidia-smi"], capture_output=True, text=True)
    return r.returncode == 0

def mac_videotoolbox_available() -> bool:
    if platform.system() != "Darwin":
        return False
    r = subprocess.run(["ffmpeg", "-hide_banner", "-encoders"], capture_output=True, text=True)
    return "h264_videotoolbox" in (r.stdout or "")

# --- Scan input files (ignore encoded/) ---
files = []
for root, dirs, names in os.walk(directory):
    dirs[:] = [d for d in dirs if d != "encoded"]
    for n in names:
        if is_video(n):
            files.append(os.path.join(root, n))

use_nvenc = cuda_available()
use_vtb = mac_videotoolbox_available()
print(f"NVENC: {use_nvenc} | VideoToolbox: {use_vtb} | OS: {platform.system()}")

for f in files:
    base = os.path.basename(f)
    stem = extract_episode_stem(base)

    if stem is None:
        stem = slugify(os.path.splitext(base)[0])
        print(f"⚠️  No se pudo extraer episodio de: {base} -> usando: {stem}")

    out = os.path.join(out_dir, f"{stem}.mkv")
    if os.path.exists(out):
        continue

    src_fps = get_src_fps(f)
    fps_str, gop, keymin, reason = pick_fps_mode(src_fps)

    vf = "hqdn3d=1.2:1.2:3:3,scale=640:480:force_original_aspect_ratio=increase,crop=640:480,setsar=1"

    # Base común (sin subs, solo v+a)
    cmd = [
        "ffmpeg", "-y",
        "-i", f,
        "-sn",
        "-map", "0:v:0",
        "-map", "0:a:0?",
        "-vf", vf,

        "-r", fps_str,
        "-g", str(gop),
        "-keyint_min", str(keymin),
        "-sc_threshold", "0",

        "-b:v", "700k",
        "-maxrate", "900k",
        "-bufsize", "1800k",

        "-c:a", "aac",
        "-b:a", "96k",
        "-ac", "2",

        "-f", "matroska",
    ]

    if use_nvenc:
        cmd += [
            "-c:v", "h264_nvenc",
            "-preset", "p4",
            "-profile:v", "baseline",
            "-bf", "0",
            "-refs", "1",
            "-rc", "vbr",
            "-rc-lookahead", "0",
            "-spatial_aq", "0",
            "-temporal_aq", "0",
        ]
        enc = "h264_nvenc"
    elif use_vtb:
        cmd += [
            "-c:v", "h264_videotoolbox",
            "-profile:v", "baseline",
        ]
        enc = "h264_videotoolbox"
    else:
        cmd += [
            "-c:v", "libx264",
            "-preset", "veryfast",
            "-tune", "fastdecode",
            "-profile:v", "baseline",
            "-level", "3.0",
            "-pix_fmt", "yuv420p",
            "-x264-params", "bframes=0:ref=1:cabac=0:weightp=0",
        ]
        enc = "libx264"

    cmd.append(out)

    print(f"Encoding {os.path.basename(out)} | src_fps={src_fps:.3f} | mode={reason} | enc={enc}")
    subprocess.run(cmd, check=True)

    os.remove(f)
    print(f"Deleted original: {os.path.basename(f)}")