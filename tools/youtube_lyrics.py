"""
youtube_lyrics.py  —  SonicCanvas companion
Watches browser tabs for a YouTube video, fetches time-stamped lyrics
from syncedlyrics (lrclib / Musixmatch / NetEase — no API key needed),
and streams each line to the SonicCanvas display at the right moment.

Install:
    pip install pygetwindow requests syncedlyrics

Usage:
    python youtube_lyrics.py
    python youtube_lyrics.py 192.168.1.42      # explicit IP
    python youtube_lyrics.py soniccanvas.local  # mDNS
"""

import sys, re, time, threading
import requests
import pygetwindow as gw
import syncedlyrics

# ── config ────────────────────────────────────────────────────────────────────
ESP32_HOST   = sys.argv[1] if len(sys.argv) > 1 else "soniccanvas.local"
ESP32_URL    = f"http://{ESP32_HOST}/text"
POLL_SEC     = 2        # how often to check browser tabs
HTTP_TIMEOUT = 3        # ESP32 request timeout

SD_W    = 96            # display width in pixels — must match firmware
CHAR_W  = 6             # default GFX font: 6 px per character
SCROLL_PPS = 30.0       # TICKER_SPEED(0.5) × ~60 fps
# ─────────────────────────────────────────────────────────────────────────────

# ── ESP32 communication ───────────────────────────────────────────────────────
def send(text):
    try:
        requests.get(ESP32_URL, params={"msg": text[:127]}, timeout=HTTP_TIMEOUT)
    except Exception:
        pass

def clear():
    try:
        requests.get(ESP32_URL, params={"msg": ""}, timeout=HTTP_TIMEOUT)
    except Exception:
        pass

# ── scroll timing (fallback for plain lyrics) ─────────────────────────────────
def scroll_time(text):
    return (len(text) * CHAR_W + SD_W) / SCROLL_PPS

# ── title parsing ─────────────────────────────────────────────────────────────
_NOISE = re.compile(
    r'\s*[\(\[]'
    r'(Official\s*(Music\s*)?Video|Lyrics?|Audio|HD|4K|MV|M/V|Visualizer|Live)'
    r'[\)\]]',
    re.IGNORECASE
)

def parse_title(yt_title):
    t = re.sub(r'\s*[-–|]\s*YouTube.*$', '', yt_title, flags=re.IGNORECASE).strip()
    t = _NOISE.sub('', t).strip()
    parts = re.split(r'\s*[-–]\s*', t, maxsplit=1)
    if len(parts) == 2:
        return parts[0].strip(), parts[1].strip()
    return t.strip(), ""

# ── LRC parser ────────────────────────────────────────────────────────────────
_LRC_LINE = re.compile(r'\[(\d+):(\d+(?:\.\d+)?)\](.*)')

def parse_lrc(lrc_text):
    """
    Convert LRC to [(display_duration_sec, line_text), ...].
    Duration for each line = gap until the next line's timestamp (clamped 2-12 s).
    """
    entries = []
    for m in _LRC_LINE.finditer(lrc_text):
        mins, secs, text = m.groups()
        ts   = int(mins) * 60 + float(secs)
        text = text.strip()
        if text:
            entries.append((ts, text))

    result = []
    for i, (ts, text) in enumerate(entries):
        if i + 1 < len(entries):
            dur = entries[i + 1][0] - ts
        else:
            dur = 5.0
        result.append((max(2.0, min(dur, 12.0)), text))
    return result

# ── lyrics fetch ──────────────────────────────────────────────────────────────
def fetch_lyrics(song, artist):
    query = f"{song} {artist}" if artist else song
    print(f'[lyrics] searching: "{query}"')

    # 1 — syncedlyrics (lrclib / Musixmatch / NetEase, no key needed)
    try:
        lrc = syncedlyrics.search(query)
        if lrc:
            lines = parse_lrc(lrc)
            if lines:
                print(f"[lyrics] synced — {len(lines)} lines")
                return lines, True   # (lines, is_synced)
    except Exception as e:
        print(f"[lyrics] syncedlyrics error: {e}")

    # 2 — lrclib.net plain lyrics (no key, always free)
    try:
        r = requests.get(
            "https://lrclib.net/api/search",
            params={"q": query},
            timeout=5,
            headers={"User-Agent": "SonicCanvas/1.0"}
        )
        if r.ok and r.json():
            plain = r.json()[0].get("plainLyrics", "")
            if plain:
                lines = [
                    (scroll_time(l), l)
                    for l in plain.split('\n')
                    if l.strip() and not re.match(r'^\[.*\]$', l.strip())
                ]
                if lines:
                    print(f"[lyrics] lrclib plain — {len(lines)} lines")
                    return lines, False
    except Exception as e:
        print(f"[lyrics] lrclib error: {e}")

    print("[lyrics] not found")
    return [], False

# ── streaming thread ──────────────────────────────────────────────────────────
_stop   = threading.Event()
_thread = None

def _stream(lines, stop_event):
    for duration, text in lines:
        if stop_event.is_set():
            break
        send(text)
        print(f"  ♪  {text}")
        stop_event.wait(timeout=duration)
    if not stop_event.is_set():
        clear()

def start_lyrics(song, artist):
    global _stop, _thread
    _stop.set()
    if _thread and _thread.is_alive():
        _thread.join(timeout=3)

    lines, synced = fetch_lyrics(song, artist)
    if not lines:
        label = f"{song}  {artist}" if artist else song
        send(label)
        return

    _stop   = threading.Event()
    _thread = threading.Thread(target=_stream, args=(lines, _stop), daemon=True)
    _thread.start()

def stop_lyrics():
    global _stop
    _stop.set()
    clear()

# ── YouTube tab watcher ───────────────────────────────────────────────────────
def find_youtube_title():
    for t in gw.getAllTitles():
        if " - YouTube" in t:
            return t
    return None

# ── main ──────────────────────────────────────────────────────────────────────
print(f"SonicCanvas lyrics  →  {ESP32_URL}")
print("No API key needed. Watching for YouTube tabs... (Ctrl-C to stop)\n")

last_raw = None

try:
    while True:
        raw = find_youtube_title()

        if raw and raw != last_raw:
            last_raw = raw
            song, artist = parse_title(raw)
            print(f'\n[yt] "{raw}"')
            print(f'     song="{song}"  artist="{artist}"')
            start_lyrics(song, artist)

        elif not raw and last_raw:
            last_raw = None
            stop_lyrics()
            print("[yt] tab closed — ticker cleared")

        time.sleep(POLL_SEC)

except KeyboardInterrupt:
    print("\nStopped.")
    stop_lyrics()
