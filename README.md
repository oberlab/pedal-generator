# Pedal-Generator für ein Fahrradkino

## Projektidee

Die Gemeinde Holzkirchen möchte einen Beamer mit Energie betreiben, die Besucherinnen und Besucher auf Fahrrädern erzeugen. Das System soll elektrische Leistung anschaulich machen und gleichzeitig als gemeinschaftliche, spielerische Installation funktionieren.

Vier Fahrräder treiben jeweils über eine Trainingsrolle einen Generator an. Die Generatorleistung wird gleichgerichtet beziehungsweise über je einen Laderegler auf das gemeinsame 12-V-System geführt. Ein LiFePO₄-Akku puffert Lastwechsel und kurze Pausen. Ein Wechselrichter versorgt Beamer, Zuspieler und gegebenenfalls einen Audioverstärker mit 230 V.

Ein ESP32 erfasst die Messwerte, eröffnet ein eigenes WLAN und stellt darin eine Weboberfläche bereit. Die Teilnehmenden öffnen diese Oberfläche auf Smartphones, Tablets oder Notebooks und sehen dort den Zustand des Gesamtsystems sowie die Leistung jedes Fahrrads.

## Ziel von Version 1

Version 1 umfasst vier vollständig vorgesehene Fahrradkanäle. Bei einer Veranstaltung müssen nicht alle vier Plätze aufgebaut oder benutzt werden. Jeder Kanal kann deshalb in der Software einzeln aktiviert oder deaktiviert werden.

Das System soll:

- bis zu vier Generatoren an einer gemeinsamen Zentraleinheit aufnehmen,
- einen Beamer über Akku und Wechselrichter unterbrechungsfrei versorgen,
- bei aussetzender Erzeugung ungefähr zehn Minuten Puffer bereitstellen,
- aktuelle Leistung und erzeugte Energie je Fahrrad messen,
- Akku-, Erzeugungs- und Verbrauchsdaten übersichtlich darstellen,
- ohne vorhandene WLAN-Infrastruktur und ohne Internetzugang funktionieren,
- für Transport und wiederholten Veranstaltungsbetrieb geeignet sein.

## Systemübersicht

```text
Bike 1 ─ Generator ─ Laderegler ─ Strommessung ─┐
Bike 2 ─ Generator ─ Laderegler ─ Strommessung ─┤
Bike 3 ─ Generator ─ Laderegler ─ Strommessung ─┼─ 12-V-Bus ─ LiFePO₄-Akku
Bike 4 ─ Generator ─ Laderegler ─ Strommessung ─┘                │
                                                                ├─ ESP32 + Messwerterfassung
                                                                └─ Wechselrichter ─ 230 V ─ Beamer
```

Der aktuelle Entwurf sieht einen 500-VA-Wechselrichter und einen 12-V-LiFePO₄-Akku vor. Die endgültige Akkukapazität ist noch zu bestätigen; im Slack-Verlauf werden sowohl 25 Ah als auch 50 Ah genannt.

## Funktionale Anforderungen

### Messung

- Vier unabhängige Generator-Eingangskanäle sind vorhanden.
- Pro aktivem Fahrrad werden aktuelle Leistung in Watt und aufsummierte Energie in Wattstunden ermittelt.
- Für korrekte Leistungswerte werden Strom und die tatsächliche Spannung des 12-V-Busses berücksichtigt.
- Akkuspannung, Akkustrom und Lade- beziehungsweise Entladezustand werden erfasst.
- Die Ausgangsleistung beziehungsweise Last des Wechselrichters soll erfasst oder aus verfügbaren Wechselrichterdaten übernommen werden.
- Das System arbeitet auch dann, wenn das Bluetooth-BMS des Akkus nicht verfügbar oder nicht auslesbar ist.

### ESP32 und Netzwerk

- Controller ist ein ESP32.
- Die Firmware wird mit PlatformIO und dem Arduino-Framework gebaut.
- Der ESP32 eröffnet den WLAN-Access-Point `Pedal-Kino`.
- Die Weboberfläche liegt im LittleFS des ESP32 und ist ohne Internet erreichbar.
- Smartphones und Notebooks verbinden sich direkt mit dem Access Point.
- Die erforderliche WLAN-Reichweite beträgt ungefähr zehn Meter.
- Die WLAN-Lösung soll keine beim Transport leicht abbrechende Antenne benötigen.

### Live-Daten

- Der ESP32 stellt den WebSocket-Endpunkt `/ws` bereit.
- Telemetrie wird spätestens einmal pro Sekunde an alle verbundenen WebSocket-Clients gesendet.
- Bei einer deutlichen Leistungsänderung wird zusätzlich sofort ein Datensatz gesendet. Der aktuelle Schwellwert im Firmwaregerüst beträgt 10 W.
- Neu verbundene Clients erhalten unmittelbar den aktuellen Zustand.
- Die Konfiguration aktiver Fahrradkanäle wird auf dem ESP32 dauerhaft gespeichert.

### Weboberfläche

Die Navigation bietet folgende Ansichten:

- **Einstellungen:** Demo-/Live-Datenquelle und aktive Fahrradplätze
- **Übersicht:** Akku, Restlaufzeit, Leistungsfluss, Gesamtleistung, Verbrauch und alle aktiven Fahrräder mit kompaktem Nadelinstrument
- **Bike 1–4:** großes Nadelinstrument, aktuelle Leistung, erzeugte Energie, Rang und Verlauf des jeweiligen Fahrrads

Die Oberfläche ist für Smartphones ausgelegt und skaliert bis zu Notebook-Bildschirmen. Sie benötigt keine externen Schriftarten, Bibliotheken oder Internetressourcen.

## Demomodus

Der Demomodus erzeugt plausible Zufallsdaten direkt in JavaScript. Es wird keine WebSocket-Verbindung aufgebaut. Dadurch kann die Oberfläche ohne ESP32 entwickelt, präsentiert und auf verschiedenen Bildschirmgrößen getestet werden.

Der Modus lässt sich unter **Einstellungen** aktivieren. Beim direkten Öffnen von `data/index.html` wird er automatisch verwendet. Wird die Seite über einen Webserver ausgeliefert, kann er zusätzlich mit `?demo=1` gestartet werden, beispielsweise:

```text
http://localhost:8000/?demo=1
```

## WebSocket-Protokoll

### Telemetrie vom ESP32

```json
{
  "type": "telemetry",
  "timestamp": 123456,
  "system": {
    "batteryPercent": 74.2,
    "batteryVoltage": 13.1,
    "batteryCurrentA": 4.8,
    "batteryPowerW": 62.9,
    "loadPowerW": 215.0,
    "remainingMinutes": 18,
    "state": "discharging",
    "connectedClients": 3
  },
  "bikes": [
    { "id": 1, "enabled": true, "powerW": 118.0, "energyWh": 42.7 },
    { "id": 2, "enabled": true, "powerW": 96.0, "energyWh": 38.1 },
    { "id": 3, "enabled": false, "powerW": 0.0, "energyWh": 0.0 },
    { "id": 4, "enabled": true, "powerW": 132.0, "energyWh": 51.4 }
  ]
}
```

`system.state` ist `charging`, `discharging` oder `balanced`. Eine unbekannte Restlaufzeit wird als `-1` übertragen.

### Konfiguration an den ESP32

```json
{
  "type": "config",
  "activeBikes": [true, true, false, true]
}
```

Die vier Positionen entsprechen Bike 1 bis Bike 4. Die Firmware speichert die Auswahl in den ESP32 Preferences.

## Projektstruktur

```text
.
├── data/              Weboberfläche für LittleFS
│   ├── index.html
│   ├── app.css
│   └── app.js
├── src/
│   └── main.cpp       ESP32-Firmware
├── slack/             Gespeicherter Slack-Verlauf und Entwurfsbilder
├── platformio.ini     PlatformIO-Konfiguration
└── README.md
```

## Bauen und aufspielen

Firmware bauen:

```bash
pio run
```

Firmware auf den ESP32 übertragen:

```bash
pio run --target upload
```

Weboberfläche in das LittleFS übertragen:

```bash
pio run --target uploadfs
```

Serielle Ausgabe öffnen:

```bash
pio device monitor
```

Nach dem Start verbindet man sich mit dem WLAN `Pedal-Kino` und öffnet `http://192.168.4.1`. Das voreingestellte Passwort steht derzeit als Entwicklungsvorgabe in `src/main.cpp` und muss vor einem öffentlichen Einsatz geändert werden.

Für einen lokalen UI-Test kann im Projektverzeichnis beispielsweise ein einfacher statischer Webserver gestartet werden:

```bash
python3 -m http.server 8000 --directory data
```

Anschließend wird `http://localhost:8000/?demo=1` geöffnet.

## Stand der Implementierung

Bereits vorhanden:

- PlatformIO-Projekt für einen generischen ESP32,
- Access Point, LittleFS-Webserver und WebSocket-Endpunkt,
- zyklischer Broadcast und Broadcast bei größeren Änderungen,
- dauerhafte Aktivierung beziehungsweise Deaktivierung der vier Fahrradkanäle,
- responsive Live-Oberfläche,
- JavaScript-Demomodus.

Noch umzusetzen:

- Treiber und Kalibrierung der eingesetzten Strom- und Spannungssensoren,
- Ermittlung des Akku-Ladezustands und einer belastbaren Restlaufzeit,
- Einbindung verfügbarer Daten des Victron-Wechselrichters,
- Zurücksetzen und Zuordnen der Energiezähler bei Teilnehmerwechseln,
- Festlegung, ob Ranglisten nur für eine Sitzung oder dauerhaft gespeichert werden,
- Verhalten bei leerem Akku, Überlast, Sensorfehler und Kommunikationsausfall.

## Offene technische Entscheidungen

- Exaktes Modell und Kapazität des LiFePO₄-Akkus
- Tatsächliche Leistungsaufnahme von Beamer, Zuspieler und Audioanlage
- Dimensionierung und Position aller Sicherungen
- Zulässige Dauer- und Spitzenströme von Generatoren, Ladereglern, Sensoren und Akku
- Direkte Messung der Wechselrichterleistung oder Nutzung einer verfügbaren Victron-Schnittstelle
- Mechanische Korrektur der ungefähr 10 mm fehlerhaften Fluchtung an der Fahrradhalterung
- Schutzart und Einsatzgrenzen bei Regen beziehungsweise Außenveranstaltungen
- Art der geschützten WLAN-Montage außerhalb oder an einer Öffnung des Metallgehäuses

## Sicherheit

Das System kombiniert hohe Gleichströme, einen Energiespeicher und berührungsgefährliche 230-V-Wechselspannung. Schaltplan, Leiterquerschnitte, Sicherungen, Schutzleiterführung, Gehäuseerdung, Zugentlastung, Berührschutz, Feuchtigkeitsschutz und Abschaltbedingungen müssen vor dem Veranstaltungsbetrieb von einer entsprechend qualifizierten Elektrofachkraft geprüft und dokumentiert werden. Die Software ersetzt keine hardwareseitige Schutzfunktion.

## Abnahmekriterien für die Softwarebasis

- Ein Smartphone kann sich ohne vorhandenes Netzwerk mit dem ESP32 verbinden und die Oberfläche öffnen.
- Die Oberfläche ist bei 360 px Breite sowie auf einem Notebook vollständig bedienbar.
- Vier Fahrradkanäle werden unterstützt und einzeln konfiguriert.
- Mehrere gleichzeitig verbundene Browser erhalten denselben Telemetrie-Broadcast.
- Ein neuer Client zeigt spätestens nach einer Sekunde aktuelle Daten.
- Eine Leistungsänderung von mindestens 10 W löst vor dem nächsten Regelintervall einen Broadcast aus.
- Die Auswahl aktiver Fahrräder bleibt nach einem Neustart erhalten.
- Der Demomodus funktioniert ohne ESP32, WebSocket und Internetzugang.
