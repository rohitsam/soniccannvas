"""
youtube_now_playing.py
Watches your browser tabs for a YouTube video and sends the title
to SonicCanvas so it scrolls across the bottom of the display.

Install:
    pip install pygetwindow requests

Usage:
    python youtube_now_playing.py
    python youtube_now_playing.py 192.168.1.42      # explicit IP
    python youtube_now_playing.py soniccanvas.local # mDNS (if enabled)
"""

import sys
import time
import re
import requests
import pygetwindow as gw

# ── config ────────────────────────────────────────────────────────────────────
ESP32_HOST  = sys.argv[1] if len(sys.argv) > 1 else "soniccanvas.local"
ESP32_URL   = f"http://{ESP32_HOST}/text"
POLL_SEC    = 2      # how often to check tabs
REFRESH_SEC = 60     # re-send even if title unchanged (keeps ticker alive)
TIMEOUT     = 3      # HTTP timeout seconds
CLEAR_SEC   = 10     # clear ticker this many seconds after sending
# ─────────────────────────────────────────────────────────────────────────────

def find_youtube_title():
    """Return the video title from any open YouTube browser tab, or None."""
    for title in gw.getAllTitles():
        if "YouTube" in title and " - YouTube" in title:
            # Format: "Video Title - YouTube"  or  "Video Title - Artist - YouTube"
            clean = re.sub(r"\s*[-–]\s*YouTube.*$", "", title).strip()
            if clean:
                return clean
    return None

def send(title):
    try:
        r = requests.get(ESP32_URL, params={"msg": title}, timeout=TIMEOUT)
        if r.status_code == 200:
            print(f"[ok] {title}")
        else:
            print(f"[err] HTTP {r.status_code}")
    except requests.exceptions.ConnectionError:
        print(f"[err] cannot reach {ESP32_HOST}")
    except requests.exceptions.Timeout:
        print("[err] timeout")

def clear():
    try:
        requests.get(ESP32_URL, params={"msg": ""}, timeout=TIMEOUT)
        print("[ok] cleared")
    except Exception:
        pass

# ── main loop ─────────────────────────────────────────────────────────────────
print(f"SonicCanvas ticker → http://{ESP32_HOST}/text")
print("Watching browser tabs for YouTube... (Ctrl-C to stop)\n")

last_title   = None
last_sent_at = 0
cleared      = False   # True once we've auto-cleared after CLEAR_SEC

try:
    while True:
        title = find_youtube_title()
        now   = time.time()

        if title:
            if title != last_title or (now - last_sent_at) >= REFRESH_SEC:
                send(title)
                last_title   = title
                last_sent_at = now
                cleared      = False
            elif not cleared and (now - last_sent_at) >= CLEAR_SEC:
                clear()
                cleared = True
        elif last_title:
            # YouTube tab closed or navigated away
            clear()
            last_title = None
            cleared    = False

        time.sleep(POLL_SEC)

except KeyboardInterrupt:
    print("\nStopped. Clearing ticker...")
    clear()
