#!/usr/bin/env python3
"""Live status and calibration logger for the Pedal-Kino ESP32.

Uses only the Python standard library and connects directly to the firmware's
WebSocket endpoint. No pip installation is required.
"""

from __future__ import annotations

import argparse
import base64
import csv
import hashlib
import json
import secrets
import socket
import struct
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, BinaryIO
from urllib.parse import urlparse


DEFAULT_URL = "ws://192.168.4.1/ws"
WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
BIKE_COUNT = 4


class WebSocketError(RuntimeError):
    """Raised for connection or protocol errors."""


def read_exact(stream: BinaryIO, length: int) -> bytes:
    chunks: list[bytes] = []
    remaining = length
    while remaining:
        chunk = stream.read(remaining)
        if not chunk:
            raise WebSocketError("Verbindung wurde geschlossen")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


class WebSocketClient:
    def __init__(self, url: str, timeout: float = 5.0) -> None:
        parsed = urlparse(url)
        if parsed.scheme != "ws" or not parsed.hostname:
            raise ValueError("Nur WebSocket-Adressen der Form ws://host[:port]/pfad werden unterstützt")
        self.host = parsed.hostname
        self.port = parsed.port or 80
        self.path = parsed.path or "/"
        if parsed.query:
            self.path += f"?{parsed.query}"
        self.timeout = timeout
        self.socket: socket.socket | None = None
        self.stream: BinaryIO | None = None

    def connect(self) -> None:
        connection = socket.create_connection((self.host, self.port), self.timeout)
        connection.settimeout(self.timeout)
        stream = connection.makefile("rwb", buffering=0)
        key = base64.b64encode(secrets.token_bytes(16)).decode("ascii")
        request = (
            f"GET {self.path} HTTP/1.1\r\n"
            f"Host: {self.host}:{self.port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        ).encode("ascii")
        stream.write(request)

        status = stream.readline().decode("latin-1").strip()
        headers: dict[str, str] = {}
        while True:
            line = stream.readline()
            if line in (b"\r\n", b"\n", b""):
                break
            name, separator, value = line.decode("latin-1").partition(":")
            if separator:
                headers[name.strip().lower()] = value.strip()

        expected_accept = base64.b64encode(
            hashlib.sha1((key + WEBSOCKET_GUID).encode("ascii")).digest()
        ).decode("ascii")
        if not status.startswith("HTTP/1.1 101"):
            stream.close()
            connection.close()
            raise WebSocketError(f"WebSocket-Handshake abgelehnt: {status or 'keine Antwort'}")
        if headers.get("sec-websocket-accept") != expected_accept:
            stream.close()
            connection.close()
            raise WebSocketError("Ungültige WebSocket-Handshake-Antwort")

        self.socket = connection
        self.stream = stream

    def close(self) -> None:
        if self.stream:
            try:
                self._send_frame(0x8, b"")
            except (OSError, WebSocketError):
                pass
            self.stream.close()
        if self.socket:
            self.socket.close()
        self.stream = None
        self.socket = None

    def _send_frame(self, opcode: int, payload: bytes) -> None:
        if not self.stream:
            raise WebSocketError("Nicht verbunden")
        mask = secrets.token_bytes(4)
        length = len(payload)
        header = bytearray([0x80 | opcode])
        if length < 126:
            header.append(0x80 | length)
        elif length <= 0xFFFF:
            header.append(0x80 | 126)
            header.extend(struct.pack("!H", length))
        else:
            header.append(0x80 | 127)
            header.extend(struct.pack("!Q", length))
        header.extend(mask)
        masked = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
        self.stream.write(bytes(header) + masked)

    def send_json(self, value: dict[str, Any]) -> None:
        self._send_frame(0x1, json.dumps(value, separators=(",", ":")).encode("utf-8"))

    def receive_text(self) -> str:
        fragments = bytearray()
        text_message = False
        while True:
            if not self.stream:
                raise WebSocketError("Nicht verbunden")
            first, second = read_exact(self.stream, 2)
            final = bool(first & 0x80)
            opcode = first & 0x0F
            masked = bool(second & 0x80)
            length = second & 0x7F
            if length == 126:
                length = struct.unpack("!H", read_exact(self.stream, 2))[0]
            elif length == 127:
                length = struct.unpack("!Q", read_exact(self.stream, 8))[0]
            mask = read_exact(self.stream, 4) if masked else b""
            payload = read_exact(self.stream, length)
            if masked:
                payload = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))

            if opcode == 0x8:
                raise WebSocketError("ESP32 hat die Verbindung geschlossen")
            if opcode == 0x9:
                self._send_frame(0xA, payload)
                continue
            if opcode == 0xA:
                continue
            if opcode == 0x1:
                fragments = bytearray(payload)
                text_message = True
            elif opcode == 0x0 and text_message:
                fragments.extend(payload)
            else:
                continue
            if final:
                return fragments.decode("utf-8")


def number(value: Any, digits: int = 2, unit: str = "") -> str:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        return "–"
    return f"{value:.{digits}f}{unit}"


def flag(value: Any) -> str:
    return "OK" if value is True else "FEHLER" if value is False else "–"


def render_status(message: dict[str, Any], url: str, clear: bool) -> None:
    system = message.get("system") or {}
    measurements = message.get("measurements") or {}
    voltage = measurements.get("batteryVoltage") or {}
    battery_current = measurements.get("batteryCurrent") or {}
    bikes = message.get("bikes") or []
    if clear and sys.stdout.isatty():
        print("\033[2J\033[H", end="")

    status = "READY" if system.get("ready") else "FAULT" if system.get("fault") else "UNBEKANNT"
    print(f"Pedal-Kino  {status}  {url}")
    print(
        f"ESP-Zeit {message.get('timestamp', '–')} ms | Messung #{measurements.get('sequence', '–')} "
        f"| Clients {system.get('connectedClients', '–')}"
    )
    print(
        f"Bus: {number(system.get('batteryVoltage'), 3, ' V')} | "
        f"ADC {voltage.get('adcRaw', '–')} [{voltage.get('adcRawMin', '–')}..{voltage.get('adcRawMax', '–')}] "
        f"| {flag(voltage.get('valid'))}"
    )
    print(
        f"Akku: {number(system.get('batteryCurrentA'), 3, ' A')} | "
        f"{number(system.get('batteryPowerW'), 1, ' W')} | "
        f"ADC {battery_current.get('adcRaw', '–')} "
        f"[{battery_current.get('adcRawMin', '–')}..{battery_current.get('adcRawMax', '–')}] "
        f"| {flag(battery_current.get('valid'))}"
    )
    print()
    print("Bike  Fahrer  Aktiv   Strom       Leistung    Energie      ADC [min..max]   Sensor")
    print("----  ------  ------  -----------  ----------  -----------  ----------------  ------")
    for index in range(BIKE_COUNT):
        bike = bikes[index] if index < len(bikes) else {}
        enabled = "ja" if bike.get("enabled") else "nein"
        raw_range = f"{bike.get('adcRaw', '–')} [{bike.get('adcRawMin', '–')}..{bike.get('adcRawMax', '–')}]"
        print(
            f"{index + 1:>4}  {bike.get('riderId', '–'):>6}  {enabled:<6}  {number(bike.get('currentA'), 3, ' A'):>11}  "
            f"{number(bike.get('powerW'), 1, ' W'):>10}  {number(bike.get('energyWh'), 3, ' Wh'):>11}  "
            f"{raw_range:<16}  {flag(bike.get('measurementValid'))}"
        )
    print(f"\nEmpfangen: {datetime.now().astimezone().isoformat(timespec='seconds')}", flush=True)


CSV_FIELDS = [
    "received_at",
    "esp_timestamp_ms",
    "sequence",
    "battery_voltage_v",
    "battery_adc_raw",
    "battery_adc_min",
    "battery_adc_max",
    "battery_current_a",
    "battery_power_w",
    "battery_current_adc_raw",
    "battery_current_adc_min",
    "battery_current_adc_max",
] + [
    f"bike{index}_{field}"
    for index in range(1, BIKE_COUNT + 1)
    for field in ("current_a", "power_w", "adc_raw", "adc_min", "adc_max", "valid")
]


def csv_row(message: dict[str, Any]) -> dict[str, Any]:
    system = message.get("system") or {}
    measurements = message.get("measurements") or {}
    voltage = measurements.get("batteryVoltage") or {}
    battery_current = measurements.get("batteryCurrent") or {}
    bikes = message.get("bikes") or []
    row: dict[str, Any] = {
        "received_at": datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
        "esp_timestamp_ms": message.get("timestamp"),
        "sequence": measurements.get("sequence"),
        "battery_voltage_v": system.get("batteryVoltage"),
        "battery_adc_raw": voltage.get("adcRaw"),
        "battery_adc_min": voltage.get("adcRawMin"),
        "battery_adc_max": voltage.get("adcRawMax"),
        "battery_current_a": system.get("batteryCurrentA"),
        "battery_power_w": system.get("batteryPowerW"),
        "battery_current_adc_raw": battery_current.get("adcRaw"),
        "battery_current_adc_min": battery_current.get("adcRawMin"),
        "battery_current_adc_max": battery_current.get("adcRawMax"),
    }
    for index in range(1, BIKE_COUNT + 1):
        bike = bikes[index - 1] if index <= len(bikes) else {}
        row.update(
            {
                f"bike{index}_current_a": bike.get("currentA"),
                f"bike{index}_power_w": bike.get("powerW"),
                f"bike{index}_adc_raw": bike.get("adcRaw"),
                f"bike{index}_adc_min": bike.get("adcRawMin"),
                f"bike{index}_adc_max": bike.get("adcRawMax"),
                f"bike{index}_valid": bike.get("measurementValid"),
            }
        )
    return row


def open_csv(path: Path) -> tuple[Any, csv.DictWriter]:
    existed = path.exists() and path.stat().st_size > 0
    handle = path.open("a", newline="", encoding="utf-8")
    writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS)
    if not existed:
        writer.writeheader()
        handle.flush()
    return handle, writer


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Pedal-Kino-Telemetrie per WebSocket anzeigen und optional als CSV speichern."
    )
    parser.add_argument("--url", default=DEFAULT_URL, help=f"WebSocket-Adresse (Standard: {DEFAULT_URL})")
    parser.add_argument("--csv", type=Path, help="alle Telemetriedatensätze an diese CSV-Datei anhängen")
    parser.add_argument("--json", action="store_true", help="JSON-Zeilen statt Statusansicht ausgeben")
    parser.add_argument("--quiet", action="store_true", help="keine Messwerte auf dem Terminal ausgeben")
    parser.add_argument("--once", action="store_true", help="nach dem ersten Telemetriedatensatz beenden")
    parser.add_argument("--duration", type=float, help="nach dieser Anzahl Sekunden Messdauer beenden")
    parser.add_argument("--count", type=int, help="nach dieser Anzahl Telemetriedatensätze beenden")
    parser.add_argument("--no-clear", action="store_true", help="Terminal zwischen Datensätzen nicht leeren")
    parser.add_argument("--timeout", type=float, default=5.0, help="Socket-Timeout in Sekunden")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    csv_handle = None
    writer = None
    if args.csv:
        args.csv.parent.mkdir(parents=True, exist_ok=True)
        csv_handle, writer = open_csv(args.csv)

    reconnect_delay = 1.0
    measurement_started_at: float | None = None
    measurement_count = 0
    try:
        while True:
            client = WebSocketClient(args.url, args.timeout)
            try:
                print(f"Verbinde mit {args.url} …", file=sys.stderr)
                client.connect()
                client.send_json({"type": "requestState"})
                reconnect_delay = 1.0
                while True:
                    message = json.loads(client.receive_text())
                    if message.get("type") != "telemetry":
                        continue
                    if measurement_started_at is None:
                        measurement_started_at = time.monotonic()
                    measurement_count += 1
                    if args.quiet:
                        pass
                    elif args.json:
                        print(json.dumps(message, ensure_ascii=False), flush=True)
                    else:
                        render_status(message, args.url, not args.no_clear)
                    if writer and csv_handle:
                        writer.writerow(csv_row(message))
                        csv_handle.flush()
                    if args.once:
                        return 0
                    if args.count is not None and measurement_count >= args.count:
                        return 0
                    if args.duration is not None and time.monotonic() - measurement_started_at >= args.duration:
                        return 0
            except (OSError, WebSocketError, json.JSONDecodeError) as error:
                print(f"Verbindung unterbrochen: {error}", file=sys.stderr)
            finally:
                client.close()
            print(f"Neuer Versuch in {reconnect_delay:.0f} s …", file=sys.stderr)
            time.sleep(reconnect_delay)
            reconnect_delay = min(reconnect_delay * 2.0, 10.0)
    except KeyboardInterrupt:
        print("\nBeendet.", file=sys.stderr)
        return 0
    finally:
        if csv_handle:
            csv_handle.close()


if __name__ == "__main__":
    raise SystemExit(main())
