#!/usr/bin/env python3
"""Create a dependency-free SVG plot from pedal_status.py ADC CSV data."""

from __future__ import annotations

import argparse
import csv
import html
from datetime import datetime
from pathlib import Path


CHANNELS = [
    ("Batteriespannung · GPIO33", "battery_adc_raw", "battery_adc_min", "battery_adc_max", "#68a9ff"),
    ("Batteriestrom · GPIO32", "battery_current_adc_raw", "battery_current_adc_min", "battery_current_adc_max", "#bd8cff"),
    ("Strom Bike 1 · GPIO36", "bike1_adc_raw", "bike1_adc_min", "bike1_adc_max", "#54e39a"),
    ("Strom Bike 2 · GPIO39", "bike2_adc_raw", "bike2_adc_min", "bike2_adc_max", "#ffd166"),
    ("Strom Bike 3 · GPIO34", "bike3_adc_raw", "bike3_adc_min", "bike3_adc_max", "#ff8c61"),
    ("Strom Bike 4 · GPIO35", "bike4_adc_raw", "bike4_adc_min", "bike4_adc_max", "#ff647c"),
]


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="ADC-CSV als SVG mit Rohwert und Min/Max-Band plotten")
    parser.add_argument("csv", type=Path, help="CSV aus tools/pedal_status.py")
    parser.add_argument("svg", type=Path, help="Zieldatei")
    return parser.parse_args()


def value(row: dict[str, str], key: str) -> float:
    return float(row[key])


def points(values: list[float], xs: list[float], y_min: float, y_max: float, top: float, height: float) -> str:
    span = max(1.0, y_max - y_min)
    return " ".join(
        f"{x:.1f},{top + height - (item - y_min) / span * height:.1f}"
        for x, item in zip(xs, values)
    )


def main() -> int:
    args = arguments()
    with args.csv.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    if len(rows) < 2:
        raise SystemExit("Zu wenige Messwerte zum Plotten")

    times = [datetime.fromisoformat(row["received_at"]) for row in rows]
    elapsed = [(item - times[0]).total_seconds() for item in times]
    width, panel_height, margin_x, margin_top = 1200, 155, 86, 70
    plot_width, plot_height = width - margin_x - 35, 96
    channels = [channel for channel in CHANNELS if all(key in rows[0] for key in channel[1:4])]
    total_height = margin_top + len(channels) * panel_height + 45
    duration = max(elapsed[-1], 1.0)
    xs = [margin_x + item / duration * plot_width for item in elapsed]
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{total_height}" viewBox="0 0 {width} {total_height}">',
        '<rect width="100%" height="100%" fill="#0c1412"/>',
        '<style>text{font-family:system-ui,sans-serif;fill:#f5fbf7}.muted{fill:#9cafaa}.grid{stroke:rgba(225,245,236,.13);stroke-width:1}.raw{fill:none;stroke-width:2}.band{opacity:.20}</style>',
        '<text x="32" y="35" font-size="22" font-weight="700">ADC-Rohwerte · 3-Minuten-Aufnahme</text>',
        f'<text class="muted" x="1165" y="35" font-size="13" text-anchor="end">{len(rows)} Datensätze · {duration:.1f} s</text>',
    ]

    for panel, (label, raw_key, min_key, max_key, color) in enumerate(channels):
        raw = [value(row, raw_key) for row in rows]
        minimum = [value(row, min_key) for row in rows]
        maximum = [value(row, max_key) for row in rows]
        y_low = min(minimum)
        y_high = max(maximum)
        padding = max(2.0, (y_high - y_low) * 0.08)
        y_low -= padding
        y_high += padding
        top = margin_top + panel * panel_height + 26
        parts.append(f'<text x="32" y="{top - 11}" font-size="15" font-weight="700">{html.escape(label)}</text>')
        parts.append(f'<text class="muted" x="1165" y="{top - 11}" font-size="12" text-anchor="end">min {min(minimum):.0f} · mittel {sum(raw)/len(raw):.1f} · max {max(maximum):.0f}</text>')
        for fraction in (0.0, 0.5, 1.0):
            y = top + plot_height * fraction
            label_value = y_high - (y_high - y_low) * fraction
            parts.append(f'<line class="grid" x1="{margin_x}" y1="{y:.1f}" x2="{margin_x + plot_width}" y2="{y:.1f}"/>')
            parts.append(f'<text class="muted" x="{margin_x - 10}" y="{y + 4:.1f}" font-size="11" text-anchor="end">{label_value:.0f}</text>')
        upper = points(maximum, xs, y_low, y_high, top, plot_height).split()
        lower = points(minimum, xs, y_low, y_high, top, plot_height).split()
        polygon = " ".join(upper + list(reversed(lower)))
        parts.append(f'<polygon class="band" fill="{color}" points="{polygon}"/>')
        parts.append(f'<polyline class="raw" stroke="{color}" points="{points(raw, xs, y_low, y_high, top, plot_height)}"/>')

    axis_y = margin_top + len(channels) * panel_height - 22
    for seconds in (0, 60, 120, 180):
        if seconds <= duration + 1:
            x = margin_x + seconds / duration * plot_width
            parts.append(f'<text class="muted" x="{x:.1f}" y="{axis_y}" font-size="12" text-anchor="middle">{seconds} s</text>')
    parts.append('</svg>')
    args.svg.parent.mkdir(parents=True, exist_ok=True)
    args.svg.write_text("\n".join(parts), encoding="utf-8")
    print(f"{args.svg}: {len(rows)} Datensätze, {duration:.1f} s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
