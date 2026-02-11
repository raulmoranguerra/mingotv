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

def extract_episode_id(filename: str) -> Optional[str]:
    m = re.search(r"s(\d{2})e(\d{2})", filename.lower())
    if not m:
        return None
    return f"s{m.group(1)}e{m.group(2)}"

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
    fps_23976 = 24000 / 1001  # 23.976023...
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
    """
    Checks whether NVENC encoder exists in ffmpeg, and nvidia-smi is present & working.
    If you have NVENC in ffmpeg but no nvidia-smi (some minimal setups), you can relax this.
    """
    if not ffmpeg_has_encoder("h264_nvenc"):
        return False
    r = subprocess.run(["nvidia-smi"], capture_output=True, text=True)
    return r.returncode == 0

def mac_videotoolbox_available():
    if platform.system() != "Darwin":
        return False
    r = subprocess.run(["ffmpeg", "-hide_banner", "-encoders"],
                    capture_output=True, text=True)
    return "h264_videotoolbox" in (r.stdout or "")

# --- Scan input files (ignore encoded/) ---
files = []
for root, dirs, names in os.walk(directory):
    dirs[:] = [d for d in dirs if d != "encoded"]
    for n in names:
        if is_video(n):
            files.append(os.path.join(root, n))

use_nvenc = cuda_available()
print(f"CUDA/NVENC available: {use_nvenc}")

for f in files:
    base = os.path.basename(f)
    episode = extract_episode_id(base)

    if episode is None:
        print(f"⚠️  No se pudo extraer sXXeYY de: {base} (se omite)")
        continue

    out = os.path.join(out_dir, f"{episode}.mkv")

    if os.path.exists(out):
        continue

    src_fps = get_src_fps(f)
    fps_str, gop, keymin, reason = pick_fps_mode(src_fps)

    # Filtro: pantalla llena 640x480 (4:3 sin franjas; 16:9 recorta arriba/abajo)
    vf = "hqdn3d=1.2:1.2:3:3,scale=640:480:force_original_aspect_ratio=increase,crop=640:480,setsar=1"

    # Base común
    cmd = [
        "ffmpeg", "-y",
        "-i", f,
        "-sn",
        "-map", "0:v:0",
        "-map", "0:a:0?",
        "-vf", vf,

        # FPS + GOP adaptativos
        "-r", fps_str,
        "-g", str(gop),
        "-keyint_min", str(keymin),
        "-sc_threshold", "0",

        # Bitrate bajo
        "-b:v", "700k",
        "-maxrate", "900k",
        "-bufsize", "1800k",

        # Audio (AAC estéreo)
        "-c:a", "aac",
        "-b:a", "96k",
        "-ac", "2",

        # Contenedor MKV
        "-f", "matroska",
    ]

    system = platform.system()

    if use_nvenc:
        # --- NVIDIA NVENC (Windows/Linux con RTX) ---
        cmd += [
            "-c:v", "h264_nvenc",
            "-preset", "p4",
            "-profile:v", "baseline",
            "-bf", "0",
            "-refs", "1",
            "-rc", "vbr",
            "-rc-lookahead", "0",
        ]
        enc = "h264_nvenc"

    elif mac_videotoolbox_available():
        # --- macOS hardware encode (VideoToolbox / Metal backend) ---
        cmd += [
            "-c:v", "h264_videotoolbox",
            "-profile:v", "baseline",
            "-b:v", "700k",
            "-maxrate", "900k",
        ]
        enc = "h264_videotoolbox"

    else:
        # --- CPU fallback ---
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

    enc = "h264_nvenc" if use_nvenc else "libx264"
    print(f"Encoding {episode}.mkv | src_fps={src_fps:.3f} | mode={reason} | br=500k | enc={enc}")
    subprocess.run(cmd, check=True)