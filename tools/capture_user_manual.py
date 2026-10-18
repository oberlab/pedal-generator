#!/usr/bin/env python3
"""Capture deterministic Pedal-Kino UI screenshots through Firefox WebDriver."""

from __future__ import annotations

import base64
import json
import subprocess
import time
import urllib.error
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "user_manual" / "screenshots"
BASE_URL = (ROOT / "data" / "index.html").as_uri() + "?demo=1"
CONFIG_URL = (ROOT / "data" / "config.html").as_uri() + "?demo=1"
PORT = 4455


def request(method: str, path: str, payload: dict | None = None):
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        f"http://127.0.0.1:{PORT}{path}", data=data, method=method,
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=20) as response:
        return json.load(response).get("value")


def wait_for_driver() -> None:
    for _ in range(50):
        try:
            request("GET", "/status")
            return
        except (OSError, urllib.error.URLError):
            time.sleep(0.1)
    raise RuntimeError("geckodriver did not start")


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    driver = subprocess.Popen(
        ["/snap/bin/geckodriver", "--port", str(PORT)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    session_id = None
    try:
        wait_for_driver()
        session = request("POST", "/session", {
            "capabilities": {"alwaysMatch": {
                "browserName": "firefox",
                "moz:firefoxOptions": {"args": ["-headless"]},
            }}
        })
        session_id = session["sessionId"]
        prefix = f"/session/{session_id}"

        captures = [
                ("overview.png", f"{BASE_URL}#overview", 1440, 1100, None),
                ("bike.png", f"{BASE_URL}#bike-1", 1440, 1100, None),
                ("ranking.png", f"{BASE_URL}#ranking", 1440, 1000, None),
                ("settings.png", CONFIG_URL, 1440, 1550, None),
                ("calibration.png", CONFIG_URL, 1440, 1800, "calibration"),
        ]
        for filename, url, width, height, special in captures:
            request("POST", prefix + "/window/rect", {
                "width": width, "height": height, "x": 0, "y": 0,
            })
            request("POST", prefix + "/url", {"url": url})
            time.sleep(1.5)
            if special == "calibration":
                request("POST", prefix + "/execute/sync", {
                    "script": """
                          document.querySelectorAll('[data-settings-pane]').forEach(
                            button => button.classList.toggle('active', button.dataset.settingsPane === 'calibration'));
                          document.querySelectorAll('.settings-pane').forEach(
                            pane => pane.classList.toggle('active', pane.id === 'settings-calibration'));
                          window.scrollTo(0, 0);
                    """,
                    "args": [],
                })
                time.sleep(0.5)
            image = request("GET", prefix + "/screenshot")
            (OUTPUT / filename).write_bytes(base64.b64decode(image))
    finally:
        if session_id:
            try:
                request("DELETE", f"/session/{session_id}")
            except Exception:
                pass
        driver.terminate()
        try:
            driver.wait(timeout=5)
        except subprocess.TimeoutExpired:
            driver.kill()


if __name__ == "__main__":
    main()
