"""Best-effort calibration backup before building a LittleFS image."""

Import("env")

import json
import os
from urllib.error import URLError
from urllib.request import Request, urlopen

from SCons.Script import COMMAND_LINE_TARGETS


def download_calibration():
    url = env.GetProjectOption("custom_calibration_url")
    data_dir = env.subst("$PROJECT_DATA_DIR")
    destination = os.path.join(data_dir, "calibration.json")
    temporary = destination + ".tmp"

    print("Calibration backup: requesting %s" % url)
    try:
        request = Request(url, headers={"Accept": "application/json"})
        with urlopen(request, timeout=2.0) as response:
            payload = response.read()
        document = json.loads(payload.decode("utf-8"))
        if document.get("format") != "pedal-kino-calibration":
            raise ValueError("unexpected calibration file format")
        os.makedirs(data_dir, exist_ok=True)
        with open(temporary, "wb") as output:
            output.write(payload)
            if not payload.endswith(b"\n"):
                output.write(b"\n")
        os.replace(temporary, destination)
        print("Calibration backup: saved to %s" % destination)
    except (OSError, URLError, ValueError, UnicodeError, json.JSONDecodeError) as error:
        try:
            if os.path.exists(temporary):
                os.remove(temporary)
        except OSError:
            pass
        print("Calibration backup: skipped (%s)" % error)


if "buildfs" in COMMAND_LINE_TARGETS or "uploadfs" in COMMAND_LINE_TARGETS:
    download_calibration()
