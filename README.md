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
                                      bidirektionaler Akkustrom ─┤
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
- Nach Eingabe eines Heim-WLANs versucht der ESP32 dieses beim Start 30 Sekunden
  lang. Bei Erfolg ist er über seine DHCP-Adresse beziehungsweise
  `http://pedal-kino.local` erreichbar; andernfalls öffnet er den Access Point
  `Pedal-Kino` als Fallback.
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

Das große Burger-Menü oben links beginnt mit der Übersicht, zeigt danach die
vier Bikes und die Rangliste:

- **Übersicht:** Akku-Leistungsfluss von -500 bis +500 W, Gesamtleistung,
  Verbrauch und alle aktiven Fahrräder mit kompaktem
  Nadelinstrument
- **Bike 1–4:** großes Nadelinstrument, aktuelle Leistung, erzeugte Energie, Rang und Verlauf des jeweiligen Fahrrads
- **Rangliste:** erzeugte Wattstunden aller abgeschlossenen und aktuell fahrenden
  Fahrer

Einstellungen, Netzwerkverwaltung, ADC-Rohwerte und Kalibrierung sind von der
Publikumsansicht getrennt und unter `http://<esp-ip>/config` erreichbar. Dort
kann auch das gespeicherte Heim-WLAN gelöscht werden; der ESP32 startet danach
neu und öffnet wieder seinen Access Point.

Die Oberfläche ist für Smartphones ausgelegt und skaliert bis zu Notebook-Bildschirmen. Sie benötigt keine externen Schriftarten, Bibliotheken oder Internetressourcen.

## Demomodus

Der Demomodus erzeugt plausible Zufallsdaten direkt in JavaScript. Es wird keine WebSocket-Verbindung aufgebaut. Dadurch kann die Oberfläche ohne ESP32 entwickelt, präsentiert und auf verschiedenen Bildschirmgrößen getestet werden.

Der Modus lässt sich unter `/config` aktivieren. Beim direkten Öffnen von `data/index.html` wird er automatisch verwendet. Wird die Seite über einen Webserver ausgeliefert, kann er zusätzlich mit `?demo=1` gestartet werden, beispielsweise:

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
    "batteryPercent": null,
    "batteryVoltage": 13.1,
    "batteryCurrentA": 12.3,
    "batteryPowerW": 161.5,
    "loadPowerW": null,
    "remainingMinutes": null,
    "state": "charging",
    "connectedClients": 3,
    "ready": true,
    "fault": false
  },
  "bikes": [
    { "id": 1, "riderId": 1, "enabled": true, "currentA": 8.99, "voltageV": 13.13, "powerW": 118.0, "energyWh": 42.7, "adcRaw": 1919, "adcRawMin": 1915, "adcRawMax": 1924, "measurementValid": true }
  ],
  "leaderboard": [
    { "riderId": 5, "bikeId": 2, "energyWh": 18.4 }
  ],
  "measurements": {
    "sequence": 810,
    "sampledAtMs": 40521,
    "batteryVoltage": { "pin": 33, "valueV": 13.13, "adcRaw": 2868, "adcRawMin": 2864, "adcRawMax": 2872, "valid": true },
    "batteryCurrent": { "pin": 32, "valueA": 12.3, "adcRaw": 1856, "adcRawMin": 1851, "adcRawMax": 1862, "valid": true, "calibrated": true }
  },
  "calibration": {
    "averagingWindowMs": 100,
    "currents": [
      { "channel": 0, "liveRaw": 1919, "points": [
        { "valueA": 0, "calibrated": true, "raw": 1550 },
        { "valueA": 1, "calibrated": false, "raw": null }
      ] }
    ],
    "voltage": { "liveRaw": 2868, "points": [
      { "valueV": 8, "calibrated": true, "raw": 1747 }
    ] }
  }
}
```

Die ADC-Werte sind Mittelwert, Minimum und Maximum aller Abtastungen in einem
nichtblockierenden 500-ms-Fenster. Die Firmware sendet den jeweils jüngsten Satz
mindestens einmal pro Sekunde. Eine Änderung von mindestens 10 W löst weiterhin
sofort einen zusätzlichen Datensatz aus.

Der aktuelle Schaltplan besitzt vier Generator-Stromsensoren und einen
Spannungsteiler für die Batteriespannung. Der nächste freie, auch bei aktivem
WLAN nutzbare ADC1-Eingang ist **GPIO32**. Die Firmware reserviert ihn für einen
bidirektionalen Batteriestromsensor. Positive Stromwerte bedeuten Laden,
negative Entladen. Erst nach mindestens zwei gespeicherten Kalibrierpunkten
werden `batteryCurrentA`, `batteryPowerW` und `system.state` ausgegeben; vorher
bleiben die Leistungswerte `null` und der Zustand `unknown`. Für 500 W an einem
12-V-System muss Sensor, Leiterbahn und Sicherung mindestens rund 42 A tragen;
praktisch ist ein bidirektionaler Sensor mit wenigstens ±50 A sinnvoll.

Die Status-LEDs entsprechen dem Schaltplan und sind aktiv LOW: GPIO18 zeigt
`READY`, GPIO19 `FAULT`. Ein Fehler wird bei fehlgeschlagenem LittleFS-/AP-Start
oder bei einem ADC-Kanal am Anschlag gesetzt. `system.ready` und `system.fault`
transportieren denselben Zustand per WebSocket.

## ADC-Kalibrierung am aufgebauten Gerät

Unter **`/config` → Kalibrieren** lassen sich die Kennlinien direkt am
aufgebauten Gerät erfassen. Für alle vier Bike-Stromsensoren ist die gemeinsam
vermessene Steigung von 43,5 ADC-Digits/A fest in der Firmware hinterlegt. Die
Oberfläche ermittelt deshalb nur noch die vier individuellen Nullpunkte
gleichzeitig über 30 Sekunden. Für den bidirektionalen Batteriestromsensor sind
-4, 0 und +4 A und für den Spannungssensor 8 und 14 V vorgesehen.
Der Bereich und alle schreibenden Kalibrierbefehle sind mit dem vorläufigen
Passwort `pedalkino` geschützt. Die Seite zeigt den
aktuellen 500-ms-Mittelwert sowie Status und gespeicherte Rohwerte.

Die gespeicherten Nullpunkte und Messpunkte liegen dauerhaft in den ESP32
Preferences. Zwischen
vorhandenen Punkten wird linear interpoliert, außerhalb der äußeren Punkte
linear extrapoliert. Fehlen Punkte, werden nur die vorhandenen verwendet; bei
null oder einem Punkt ergänzt die theoretische Schaltungssteigung die
Umrechnung. Dadurch bleibt die Messung auch während einer schrittweisen
Kalibrierung verfügbar.

Über **Kalibrierdatei herunterladen** oder direkt über
`http://<esp-ip>/calibration.json` lässt sich die aktuelle Kalibrierung als
JSON-Datei sichern. Der HTTP-Endpunkt liefert ausschließlich gespeicherte
Kalibrierpunkte und setzt einen Download-Dateinamen.

### Messmittel

- kalibriertes Multimeter für die tatsächlich anliegende Spannung,
- Labornetzteil mit Strombegrenzung,
- für die Stromkurven eine elektronische Last oder ausreichend dimensionierte
  Leistungswiderstände sowie idealerweise ein Referenz-Shunt/Strommessgerät,
- kurze, ausreichend dimensionierte Leitungen und die vorgesehenen Sicherungen.

Die Strombegrenzungsanzeige eines Labornetzteils ist kein ausreichend genauer
Strom-Sollwert. Der Strom wird durch die Last eingestellt und mit dem
Referenzmessgerät erfasst.

### Spannungsmessung GPIO33

1. Akku und alle Generatoren abklemmen. Den ESP32 zunächst über USB versorgen.
2. Labornetzteil mit 0 V an J1 `BAT+`/GND anschließen; Polarität vor dem
   Einschalten prüfen. Der Messpunkt `BAT_F` liegt hinter F1 und vor D1.
3. Nacheinander 8 und 14 V einstellen und die tatsächliche Spannung mit
   dem Multimeter direkt an J1 prüfen.
4. Jeden Wert stabilisieren lassen und unter **`/config` → Kalibrieren →
   Batteriespannung** den passenden Punkt messen. Jede Punktmessung läuft
   30 Sekunden und speichert danach den Mittelwert der 500-ms-Messfenster.
   Nicht benötigte oder noch nicht vermessene Punkte können unkalibriert
   bleiben.

### Strommessung GPIO36/39/34/35

Die Hall-Strombahnen der ACS712 sind galvanisch von ihren ADC-Ausgängen
getrennt. Die Steigung ist fest auf 43,5 ADC-Digits/A eingestellt; die
kanalabhängigen Nullpunkte werden gemeinsam bestimmt.

1. Platine und alle ACS712 mit 5 V versorgen, durch ihre Strompfade aber keinen
   Strom schicken.
2. Unter **`/config` → Kalibrieren** die gemeinsame Nullpunktmessung
   starten. Die Firmware sammelt 30 Sekunden lang gleichzeitig die
   500-ms-Mittelwerte aller vier Kanäle und speichert deren Mittelwerte.
3. Während der Messung keine Generatoren bewegen und sicherstellen, dass durch
   wirklich alle vier Sensoren 0 A fließen.
4. Den Nullpunkt nach 15–20 Minuten Erwärmung bei Bedarf wiederholen.
   Der ACS712 ist temperatur- und 5-V-versorgungsabhängig. Ist die Drift größer
   als die gewünschte Genauigkeit, sollte vor jeder Veranstaltung ein
   Nullabgleich erfolgen oder ein präziserer Sensor gewählt werden.

### Bidirektionale Batteriestrommessung GPIO32

GPIO32 ist in der Firmware als nächster freier ADC1-Kanal vorgesehen. Der noch
auszuwählende Sensor muss in beiden Richtungen messen und samt Strompfad für
mindestens 500 W beziehungsweise etwa 42 A bei 12 V ausgelegt sein. Vor dem
Anschluss muss sein Analogausgang auf den ESP32-Bereich von 0 bis 3,3 V begrenzt
werden. Zuerst den Nullpunkt ohne Batteriestrom speichern, danach mindestens je
einen genau gemessenen Lade- und Entladestrom. Die vorzeichenrichtige Montage
ist erreicht, wenn Ladestrom positiv und Entladestrom negativ angezeigt wird.

Anfangs fahren Fahrer 1 bis 4 auf Bike 1 bis 4. **Nächster Fahrer** übernimmt
das Ergebnis des bisherigen Fahrers in die Rangliste, vergibt die nächste freie
Fahrernummer und beginnt auf diesem Bike wieder bei 0 Wh. Der WebSocket-Befehl
dazu lautet beispielsweise `{ "type": "nextRider", "bike": 1 }`. Die
Fahrersitzung und Rangliste bleiben bis zum nächsten Neustart im RAM des ESP32;
die letzten 32 abgeschlossenen Fahrer werden geführt.

Zum Beobachten genügt beispielsweise:

```bash
wscat -c ws://192.168.4.1/ws
```

Komfortabler ist der mitgelieferte Python-Monitor. Er benötigt keine externen
Python-Pakete:

```bash
python3 tools/pedal_status.py
```

Die Adresse kann für ein anderes Netz überschrieben werden. Für eine
Kalibrierreihe schreibt `--csv` jeden empfangenen Datensatz fortlaufend weg:

```bash
python3 tools/pedal_status.py --url ws://192.168.4.1/ws \
  --csv messungen/kalibrierung.csv
```

Weitere nützliche Betriebsarten sind `--once` für genau einen Datensatz,
`--json` für unveränderte JSON-Zeilen und `--no-clear` für eine fortlaufende
Terminalausgabe. Abbruch erfolgt mit `Ctrl+C`; nach einem Verbindungsabbruch
verbindet sich das Programm automatisch erneut.

Für jede Kalibrierreihe sollten Kanal, Referenzwert, Rohwert-Mittel, Rohwert-
Minimum/-Maximum, 5-V-Versorgung und Sensortemperatur als CSV festgehalten
werden. Anschließend werden unabhängige Zwischenpunkte geprüft; sie dürfen nicht
dieselben Punkte sein, aus denen die Kurve erzeugt wurde.

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
│   ├── config.html
│   ├── app.css
│   └── app.js
├── qr_codes/          WLAN-, Live- und Admin-QR-Codes (PNG/SVG)
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

Vor `buildfs` und `uploadfs` versucht PlatformIO automatisch, die aktuelle
Kalibrierdatei vom ESP32 herunterzuladen und als `data/calibration.json` in das
LittleFS-Abbild aufzunehmen. Für `esp32dev` wird standardmäßig
`http://192.168.4.1/calibration.json`, für `esp32dev_ota`
`http://192.168.10.31/calibration.json` verwendet. Ist der ESP32 nicht
erreichbar oder liefert er kein gültiges Kalibrierformat, erscheint lediglich
eine Meldung; der Build läuft weiter und eine bereits vorhandene Sicherung wird
nicht überschrieben. Die Adresse lässt sich in `platformio.ini` über
`custom_calibration_url` ändern.

Serielle Ausgabe öffnen:

```bash
pio device monitor
```

Nach dem einmaligen USB-Upload der OTA-fähigen Firmware kann ein Update im
Heimnetz über `192.168.10.31` erfolgen:

```bash
pio run -e esp32dev_ota --target upload
pio run -e esp32dev_ota --target uploadfs
```

Der OTA-Hostname ist `pedal-kino`, Port 3232. Das derzeitige
Entwicklungskennwort entspricht dem Access-Point-Kennwort und muss vor einem
öffentlichen Einsatz geändert werden.

Nach dem Start verbindet man sich mit dem WLAN `Pedal-Kino`. Ein DNS-Captive-
Portal beantwortet alle Hostnamen mit der AP-Adresse und leitet die üblichen
Portal-Prüfungen von Android, iOS/macOS, Windows und Firefox auf die normale
Live-Anzeige um. Falls sich das Portal nicht automatisch öffnet, lautet die
Adresse `http://192.168.4.1`; die Administration liegt unter
`http://192.168.4.1/config`. Passende WLAN-, Live- und Admin-QR-Codes liegen in
`qr_codes/`. Das voreingestellte Passwort steht derzeit als Entwicklungsvorgabe
in `src/main.cpp` und muss vor einem öffentlichen Einsatz geändert werden.

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
- Hardware-Abstraktion für vier ACS712-Kanäle, Batteriespannung und den
  bidirektionalen Batteriestromkanal auf GPIO32,
- ADC-Rohdiagnose und kalibrierte Messwerte im WebSocket-Datenstrom.
- Lade-/Entladeinstrument mit ±500 W sowie Fahrerwechsel und Wh-Rangliste.

Noch umzusetzen:

- Aufnahme und Eintragung der gerätespezifischen ADC-Kalibrierpunkte,
- Ermittlung des Akku-Ladezustands und einer belastbaren Restlaufzeit,
- Einbindung verfügbarer Daten des Victron-Wechselrichters,
- dauerhafte Zuordnung der Energiezähler zu Teilnehmern,
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
