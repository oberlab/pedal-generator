#!/usr/bin/env python3
"""Generate the printable Pedal-Kino access QR codes."""

from pathlib import Path

import qrcode
from qrcode.image.svg import SvgPathImage


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "qr_codes"
ERROR_CORRECTION = qrcode.constants.ERROR_CORRECT_M

CODES = {
    "wifi_pedal_kino": "WIFI:T:WPA;S:Pedal-Kino;P:pedalkino;;",
    "live_anzeige": "http://192.168.4.1/",
    "admin_config": "http://192.168.4.1/config",
}


def make_qr(payload: str) -> qrcode.QRCode:
    qr = qrcode.QRCode(
        version=None,
        error_correction=ERROR_CORRECTION,
        box_size=16,
        border=4,
    )
    qr.add_data(payload)
    qr.make(fit=True)
    return qr


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for name, payload in CODES.items():
        qr = make_qr(payload)
        qr.make_image(fill_color="black", back_color="white").save(
            OUTPUT / f"{name}.png"
        )
        qr.make_image(image_factory=SvgPathImage).save(OUTPUT / f"{name}.svg")


if __name__ == "__main__":
    main()
