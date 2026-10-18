# QR-Codes

- `wifi_pedal_kino`: verbindet mit dem WPA2-WLAN `Pedal-Kino` (Passwort `pedalkino`)
- `live_anzeige`: öffnet `http://192.168.4.1/`
- `admin_config`: öffnet den Administrationsbereich unter `http://192.168.4.1/config`

Jeder Code liegt als PNG für die direkte Verwendung und als skalierbares SVG
für den Druck vor. Neu erzeugen lassen sie sich mit:

```bash
python3 tools/generate_qr_codes.py
```
