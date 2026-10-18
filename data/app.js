(() => {
  "use strict";

  const BIKE_COUNT = 4;
  const HISTORY_LENGTH = 60;
  const DEMO_INTERVAL_MS = 900;
  const MAX_POWER_W = 200;

  const state = {
    demoMode: new URLSearchParams(window.location.search).get("demo") === "1" || window.location.protocol === "file:",
    lastUpdate: null,
    system: {
      batteryPercent: 0,
      batteryVoltage: 0,
      batteryCurrentA: 0,
      batteryPowerW: 0,
      loadPowerW: 0,
      remainingMinutes: -1,
      state: "balanced",
      connectedClients: 0,
    },
    bikes: Array.from({ length: BIKE_COUNT }, (_, index) => ({
      id: index + 1,
      riderId: index + 1,
      riderName: "",
      enabled: true,
      currentA: 0,
      voltageV: 0,
      powerW: 0,
      energyWh: 0,
      history: Array(HISTORY_LENGTH).fill(0),
      demoTarget: 30 + Math.random() * 55,
    })),
    network: { mode: null, ssid: null, ip: null, hostname: null },
    measurements: { sequence: null, sampledAtMs: null, batteryVoltage: null, batteryCurrent: null },
    calibration: { averagingWindowMs: 500, currentRawPerAmp: 43.5, currentZeroCalibration: null, currents: [], voltage: null, batteryCurrent: null },
    leaderboard: [],
    nextRiderId: BIKE_COUNT + 1,
  };

  let activeView = "overview";
  let selectedBike = 1;
  let socket = null;
  let reconnectTimer = null;
  let reconnectDelayMs = 1000;
  let demoTimer = null;
  let toastTimer = null;
  let calibrationPassword = "";

  const elements = {
    connection: document.querySelector("#connection-status"),
    connectionLabel: document.querySelector("#connection-status .status-label"),
    menuToggle: document.querySelector("#menu-toggle"),
    navigation: document.querySelector("#main-navigation"),
    batteryFlowGauge: document.querySelector("#battery-flow-gauge"),
    batteryPower: document.querySelector("#battery-power"),
    batteryVoltage: document.querySelector("#battery-voltage"),
    batteryCurrent: document.querySelector("#battery-current"),
    flowBadge: document.querySelector("#flow-badge"),
    generatedPower: document.querySelector("#generated-power"),
    loadPower: document.querySelector("#load-power"),
    clientCount: document.querySelector("#client-count"),
    bikeGrid: document.querySelector("#bike-grid"),
    bikeDetail: document.querySelector("#bike-detail"),
    rankingList: document.querySelector("#ranking-list"),
    bikeSettings: document.querySelector("#bike-settings"),
    demoMode: document.querySelector("#demo-mode"),
    toast: document.querySelector("#toast"),
    wifiSsid: document.querySelector("#wifi-ssid"),
    wifiPassword: document.querySelector("#wifi-password"),
    networkMode: document.querySelector("#network-mode"),
    networkSsid: document.querySelector("#network-ssid"),
    networkAddress: document.querySelector("#network-address"),
    adcRawValues: document.querySelector("#adc-raw-values"),
    calibrationSensors: document.querySelector("#calibration-sensors"),
  };

  function number(value, digits = 0) {
    return new Intl.NumberFormat("de-DE", {
      minimumFractionDigits: digits,
      maximumFractionDigits: digits,
    }).format(Number.isFinite(value) ? value : 0);
  }

  function numberOrDash(value, digits = 0) {
    return Number.isFinite(value) ? number(value, digits) : "–";
  }

  function clamp(value, minimum, maximum) {
    return Math.min(maximum, Math.max(minimum, value));
  }

  function escapeText(value) {
    return String(value)
      .replaceAll("&", "&amp;")
      .replaceAll("<", "&lt;")
      .replaceAll(">", "&gt;")
      .replaceAll('"', "&quot;")
      .replaceAll("'", "&#039;");
  }

  function setConnectionStatus(connectionState, label) {
    elements.connection.dataset.state = connectionState;
    elements.connectionLabel.textContent = label;
  }

  function showToast(message) {
    elements.toast.textContent = message;
    elements.toast.classList.add("show");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => elements.toast.classList.remove("show"), 2200);
  }

  function sparkline(history, label) {
    const width = 300;
    const height = 100;
    const maximum = Math.max(MAX_POWER_W, ...history, 1);
    const points = history.map((value, index) => {
      const x = (index / Math.max(1, history.length - 1)) * width;
      const y = height - (clamp(value, 0, maximum) / maximum) * (height - 8);
      return `${x.toFixed(1)},${y.toFixed(1)}`;
    }).join(" ");
    const area = `M 0,${height} L ${points.replaceAll(" ", " L ")} L ${width},${height} Z`;
    return `<svg class="sparkline" viewBox="0 0 ${width} ${height}" preserveAspectRatio="none" role="img" aria-label="${escapeText(label)}"><path class="area" d="${area}"></path><polyline points="${points}"></polyline></svg>`;
  }

  function needleGauge(powerW, bikeId, compact = false) {
    const boundedPowerW = clamp(powerW, 0, MAX_POWER_W);
    const angle = -120 + (boundedPowerW / MAX_POWER_W) * 240;
    const arcPoint = (value, radius = 72) => {
      const pointAngle = (-120 + (value / MAX_POWER_W) * 240) * Math.PI / 180;
      return {
        x: 90 + Math.sin(pointAngle) * radius,
        y: 87 - Math.cos(pointAngle) * radius,
      };
    };
    const arcPath = (startValue, endValue) => {
      const start = arcPoint(startValue);
      const end = arcPoint(endValue);
      const arcDegrees = ((endValue - startValue) / MAX_POWER_W) * 240;
      const largeArc = arcDegrees > 180 ? 1 : 0;
      return `M ${start.x.toFixed(1)} ${start.y.toFixed(1)} A 72 72 0 ${largeArc} 1 ${end.x.toFixed(1)} ${end.y.toFixed(1)}`;
    };
    const tickStep = MAX_POWER_W / 10;
    const tickValues = compact
      ? Array.from({ length: 6 }, (_, index) => index * tickStep * 2)
      : Array.from({ length: 11 }, (_, index) => index * tickStep);
    const ticks = tickValues.map((tickValue) => {
      const tickAngle = -120 + (tickValue / MAX_POWER_W) * 240;
      const radians = tickAngle * Math.PI / 180;
      const outerRadius = 72;
      const innerRadius = tickValue % (tickStep * 2) === 0 ? 61 : 65;
      const x1 = 90 + Math.sin(radians) * innerRadius;
      const y1 = 87 - Math.cos(radians) * innerRadius;
      const x2 = 90 + Math.sin(radians) * outerRadius;
      const y2 = 87 - Math.cos(radians) * outerRadius;
      return `<line x1="${x1.toFixed(1)}" y1="${y1.toFixed(1)}" x2="${x2.toFixed(1)}" y2="${y2.toFixed(1)}"></line>`;
    }).join("");
    const labels = compact ? "" : Array.from({ length: 6 }, (_, index) => index * tickStep * 2).map((tickValue) => {
      const tickAngle = -120 + (tickValue / MAX_POWER_W) * 240;
      const radians = tickAngle * Math.PI / 180;
      const x = 90 + Math.sin(radians) * 52;
      const y = 90 - Math.cos(radians) * 52;
      return `<text x="${x.toFixed(1)}" y="${y.toFixed(1)}">${tickValue}</text>`;
    }).join("");

    return `
      <svg class="needle-gauge ${compact ? "compact" : "large"}" viewBox="0 0 180 130" role="img" aria-label="Bike ${bikeId}: ${number(powerW)} Watt">
        <path class="gauge-track" d="${arcPath(0, MAX_POWER_W)}"></path>
        <path class="gauge-zone low" d="${arcPath(0, MAX_POWER_W * .2)}"></path>
        <path class="gauge-zone good" d="${arcPath(MAX_POWER_W * .2, MAX_POWER_W * .8)}"></path>
        <path class="gauge-zone peak" d="${arcPath(MAX_POWER_W * .8, MAX_POWER_W)}"></path>
        <g class="gauge-ticks">${ticks}</g>
        <g class="gauge-labels">${labels}</g>
        <g class="gauge-needle" style="transform: rotate(${angle}deg)">
          <path d="M 87 87 L 90 25 L 93 87 Z"></path>
        </g>
        <circle class="gauge-hub" cx="90" cy="87" r="8"></circle>
      </svg>`;
  }

  function batteryFlowGauge(powerW) {
    const maximum = 500;
    const available = Number.isFinite(powerW);
    const bounded = clamp(available ? powerW : 0, -maximum, maximum);
    const angle = (bounded / maximum) * 110;
    const arcPoint = (value, radius = 86) => {
      const radians = (-110 + ((value + maximum) / (2 * maximum)) * 220) * Math.PI / 180;
      return { x: 110 + Math.sin(radians) * radius, y: 100 - Math.cos(radians) * radius };
    };
    const arcPath = (startValue, endValue) => {
      const start = arcPoint(startValue);
      const end = arcPoint(endValue);
      return `M ${start.x.toFixed(1)} ${start.y.toFixed(1)} A 86 86 0 0 1 ${end.x.toFixed(1)} ${end.y.toFixed(1)}`;
    };
    const ticks = [-500, -250, 0, 250, 500].map((value) => {
      const outer = arcPoint(value, 86);
      const inner = arcPoint(value, 74);
      const label = arcPoint(value, 62);
      return `<line x1="${inner.x.toFixed(1)}" y1="${inner.y.toFixed(1)}" x2="${outer.x.toFixed(1)}" y2="${outer.y.toFixed(1)}"></line><text x="${label.x.toFixed(1)}" y="${(label.y + 3).toFixed(1)}">${Math.abs(value)}</text>`;
    }).join("");
    return `
      <svg class="battery-flow-svg" viewBox="0 0 220 145" role="img" aria-label="Batterieleistung ${available ? `${number(bounded)} Watt` : "nicht verfügbar"}">
        <path class="battery-flow-discharge" d="${arcPath(-maximum, 0)}"></path>
        <path class="battery-flow-charge" d="${arcPath(0, maximum)}"></path>
        <g class="battery-flow-ticks">${ticks}</g>
        <g class="battery-flow-needle ${available ? "" : "unavailable"}" style="transform: rotate(${angle}deg)"><path d="M 106 100 L 110 22 L 114 100 Z"></path></g>
        <circle class="gauge-hub" cx="110" cy="100" r="8"></circle>
        <text class="battery-flow-side discharge" x="20" y="138">Entladen</text>
        <text class="battery-flow-side charge" x="200" y="138" text-anchor="end">Laden</text>
      </svg>`;
  }

  function flowCopy(flowState) {
    if (flowState === "charging") return "Akku wird geladen";
    if (flowState === "discharging") return "Akku wird entladen";
    if (flowState === "balanced") return "Erzeugung und Verbrauch im Gleichgewicht";
    return "Akkustrom nicht gemessen";
  }

  function riderLabel(rider) {
    return rider.riderName || `Fahrer ${rider.riderId}`;
  }

  function updateOverview() {
    const generatedPowerW = state.bikes.reduce((sum, bike) => sum + (bike.enabled ? bike.powerW : 0), 0);
    elements.batteryFlowGauge.innerHTML = batteryFlowGauge(state.system.batteryPowerW);
    elements.batteryPower.textContent = numberOrDash(state.system.batteryPowerW);
    elements.batteryVoltage.textContent = numberOrDash(state.system.batteryVoltage, 2);
    elements.batteryCurrent.textContent = numberOrDash(state.system.batteryCurrentA, 2);
    elements.flowBadge.dataset.state = state.system.state;
    elements.flowBadge.textContent = flowCopy(state.system.state);
    elements.generatedPower.textContent = number(generatedPowerW);
    elements.loadPower.textContent = numberOrDash(state.system.loadPowerW);
    elements.clientCount.textContent = number(state.system.connectedClients);
    elements.bikeGrid.innerHTML = state.bikes.map((bike) => `
      <article class="bike-card panel ${bike.enabled ? "" : "disabled"}" data-bike="${bike.id}" tabindex="${bike.enabled ? "0" : "-1"}" aria-label="Bike ${bike.id}, ${bike.enabled ? `${number(bike.powerW)} Watt` : "deaktiviert"}">
        <div class="bike-card-head"><div class="bike-identity"><span class="bike-number">${bike.id}</span><div class="bike-rider"><span class="rider-name">${escapeText(riderLabel(bike))}</span><small class="${bike.riderName ? "" : "placeholder"}">${bike.riderName ? `Fahrer ${bike.riderId}` : "Ohne Namen"}</small><button type="button" class="rider-name-edit" data-rider-name="${bike.riderId}">Name ändern</button></div></div><span class="bike-state">${bike.enabled ? "Aktiv" : "Pausiert"}</span></div>
        ${needleGauge(bike.enabled ? bike.powerW : 0, bike.id, true)}
        <div class="bike-power">${bike.enabled ? number(bike.powerW) : "–"} <small>W</small></div>
        <div class="bike-energy-row"><p class="bike-energy">${number(bike.energyWh, 1)} Wh erzeugt</p><button class="energy-reset" type="button" data-next-rider="${bike.id}">Nächster Fahrer</button></div>
        ${sparkline(bike.history, `Leistungsverlauf Bike ${bike.id}`)}
      </article>
    `).join("");

    elements.bikeGrid.querySelectorAll(".bike-card:not(.disabled)").forEach((card) => {
      const open = () => navigate(`bike-${card.dataset.bike}`);
      card.addEventListener("click", (event) => {
        if (!event.target.closest("[data-next-rider], [data-rider-name]")) open();
      });
      card.addEventListener("keydown", (event) => {
        if (!event.target.closest("[data-next-rider], [data-rider-name]") && (event.key === "Enter" || event.key === " ")) open();
      });
    });
    bindNextRiderButtons(elements.bikeGrid);
    bindRiderNameButtons(elements.bikeGrid);
  }

  function updateBikeDetail() {
    const bike = state.bikes[selectedBike - 1];
    if (!bike) return;
    const effort = clamp((bike.powerW / MAX_POWER_W) * 100, 0, 100);
    const rank = rankingEntries().findIndex((entry) => entry.riderId === bike.riderId) + 1;

    elements.bikeDetail.innerHTML = `
      <div class="bike-detail-header">
        <div><p class="eyebrow">Bike ${bike.id} · Fahrer ${bike.riderId}</p><h1>${escapeText(riderLabel(bike))}</h1><button type="button" class="rider-name-edit detail-name-edit" data-rider-name="${bike.riderId}">Name ändern</button></div>
        <span class="live-chip">● ${bike.enabled ? "Live" : "Deaktiviert"}</span>
      </div>
      <div class="detail-grid">
        <article class="power-stage panel">
          <div class="power-stage-copy"><p class="eyebrow">Aktuelle Leistung</p><div class="giant-power">${bike.enabled ? number(bike.powerW) : "–"}<small> W</small></div></div>
          ${needleGauge(bike.enabled ? bike.powerW : 0, bike.id)}
          <div><div class="effort-bar" aria-label="Leistung relativ zu 100 Watt"><span style="width:${effort}%"></span></div></div>
        </article>
        <div class="detail-side">
          <article class="detail-metric panel"><small>Erzeugte Energie</small><strong>${number(bike.energyWh, 1)} Wh</strong><button class="energy-reset detail-reset" type="button" data-next-rider="${bike.id}">Nächster Fahrer</button></article>
          <article class="detail-metric panel"><small>Strom · Busspannung</small><strong>${numberOrDash(bike.currentA, 2)} A · ${numberOrDash(bike.voltageV, 2)} V</strong></article>
          <article class="detail-metric panel"><small>Aktueller Rang</small><strong>${rank > 0 ? `${rank}. Platz` : "–"}</strong></article>
        </div>
        <article class="history-panel panel"><h2>Leistung der letzten Minute</h2>${sparkline(bike.history, `Leistungsverlauf Bike ${bike.id}`)}</article>
      </div>
    `;
    bindNextRiderButtons(elements.bikeDetail);
    bindRiderNameButtons(elements.bikeDetail);
  }

  function nextRider(bikeId) {
    const bike = state.bikes[bikeId - 1];
    if (!bike || !window.confirm(`Fahrer ${bike.riderId} abschließen und den nächsten Fahrer starten?`)) return;
    if (state.demoMode) {
      state.leaderboard.push({ riderId: bike.riderId, riderName: bike.riderName, bikeId, energyWh: bike.energyWh });
      bike.riderId = state.nextRiderId++;
      bike.riderName = "";
      bike.energyWh = 0;
      render();
      showToast(`Bike ${bikeId}: Fahrer ${bike.riderId} gestartet`);
      return;
    }
    if (socket?.readyState !== WebSocket.OPEN) {
      showToast("Keine Verbindung zum ESP32");
      return;
    }
    socket.send(JSON.stringify({ type: "nextRider", bike: bikeId }));
    showToast(`Bike ${bikeId}: nächster Fahrer wird gestartet`);
  }

  function bindNextRiderButtons(container) {
    container.querySelectorAll("[data-next-rider]").forEach((button) => {
      button.addEventListener("click", (event) => {
        event.stopPropagation();
        nextRider(Number(button.dataset.nextRider));
      });
    });
  }

  function bindRiderNameButtons(container) {
    container.querySelectorAll("[data-rider-name]").forEach((button) => {
      button.addEventListener("click", (event) => {
        event.stopPropagation();
        editRiderName(Number(button.dataset.riderName));
      });
    });
  }

  function rankingEntries() {
    return [
      ...state.leaderboard.map((entry) => ({ ...entry, active: false })),
      ...state.bikes.map((bike) => ({ riderId: bike.riderId, riderName: bike.riderName, bikeId: bike.id, energyWh: bike.energyWh, active: true })),
    ].sort((left, right) => right.energyWh - left.energyWh || left.riderId - right.riderId);
  }

  function updateRanking() {
    const entries = rankingEntries();
    elements.rankingList.innerHTML = entries.length ? entries.map((entry, index) => `
      <article class="ranking-entry panel">
        <div class="ranking-position">${index + 1}.</div>
        <div class="ranking-rider"><strong>${escapeText(entry.riderName || `Fahrer ${entry.riderId}`)}</strong><small>Fahrer ${entry.riderId} · Bike ${entry.bikeId}</small><button type="button" class="rider-name-edit" data-rider-name="${entry.riderId}">Name ändern</button></div>
        <div class="ranking-energy">${number(entry.energyWh, 1)} <small>Wh</small></div>
        ${entry.active ? '<span class="ranking-live">fährt gerade</span>' : '<span class="ranking-bike">abgeschlossen</span>'}
      </article>`).join("") : '<div class="ranking-empty panel">Noch keine Fahrergebnisse vorhanden.</div>';
    bindRiderNameButtons(elements.rankingList);
  }

  function editRiderName(riderId) {
    const entry = rankingEntries().find((candidate) => candidate.riderId === riderId);
    if (!entry) return;
    const entered = window.prompt(`Name für Fahrer ${riderId}:`, entry.riderName || "");
    if (entered === null) return;
    const name = entered.trim().slice(0, 31);
    if (state.demoMode) {
      state.bikes.filter((bike) => bike.riderId === riderId).forEach((bike) => { bike.riderName = name; });
      state.leaderboard.filter((result) => result.riderId === riderId).forEach((result) => { result.riderName = name; });
      render();
      return;
    }
    if (socket?.readyState !== WebSocket.OPEN) {
      showToast("Keine Verbindung zum ESP32");
      return;
    }
    socket.send(JSON.stringify({ type: "riderName", riderId, name }));
    showToast(name ? "Fahrername gespeichert" : "Fahrername entfernt");
  }

  function resetRanking() {
    if (!window.confirm("Wirklich die gesamte Rangliste, alle Fahrernamen, Fahrerzähler und erzeugten Wh zurücksetzen?")) return;
    if (!state.demoMode && socket?.readyState !== WebSocket.OPEN) {
      showToast("Keine Verbindung zum ESP32");
      return;
    }
    state.leaderboard = [];
    state.nextRiderId = BIKE_COUNT + 1;
    state.bikes.forEach((bike, index) => {
      bike.riderId = index + 1;
      bike.riderName = "";
      bike.energyWh = 0;
      bike.history = Array(HISTORY_LENGTH).fill(0);
    });
    if (!state.demoMode) socket.send(JSON.stringify({ type: "resetRanking" }));
    render();
    showToast("Rangliste und Fahrerdaten zurückgesetzt");
  }

  function updateNavigation() {
    document.querySelectorAll(".view-nav button").forEach((button) => {
      button.classList.toggle("active", button.dataset.view === activeView);
      const match = button.dataset.view.match(/^bike-(\d)$/);
      button.classList.toggle("disabled", Boolean(match && !state.bikes[Number(match[1]) - 1].enabled));
    });
  }

  function updateSettings() {
    elements.demoMode.checked = state.demoMode;
    elements.bikeSettings.innerHTML = state.bikes.map((bike) => `
      <label class="bike-toggle"><input type="checkbox" data-bike-enabled="${bike.id}" ${bike.enabled ? "checked" : ""}><span>Bike ${bike.id}</span></label>
    `).join("");
  }

  function updateNetworkInfo() {
    elements.networkMode.textContent = state.network.mode === "station" ? "Heim-WLAN" :
      state.network.mode === "accessPoint" ? "Eigener Access Point" : "–";
    elements.networkSsid.textContent = state.network.ssid || "–";
    elements.networkAddress.textContent = state.network.hostname || state.network.ip || "–";
  }

  function updateAdcRawValues() {
    const voltage = state.measurements.batteryVoltage || {};
    const batteryCurrent = state.measurements.batteryCurrent || {};
    const rows = [{
      name: "Batteriespannung", pin: 33, raw: voltage.adcRaw,
      min: voltage.adcRawMin, max: voltage.adcRawMax, valid: voltage.valid,
    }, {
      name: "Batteriestrom", pin: 32, raw: batteryCurrent.adcRaw,
      min: batteryCurrent.adcRawMin, max: batteryCurrent.adcRawMax,
      valid: batteryCurrent.valid,
    }, ...state.bikes.map((bike, index) => ({
      name: `Strom Bike ${index + 1}`,
      pin: [36, 39, 34, 35][index],
      raw: bike.adcRaw,
      min: bike.adcRawMin,
      max: bike.adcRawMax,
      valid: bike.measurementValid,
    }))];
    elements.adcRawValues.innerHTML = rows.map((row) => `
      <tr>
        <td>${row.name}</td><td>${row.pin}</td><td>${row.raw ?? "–"}</td>
        <td>${row.min ?? "–"}</td><td>${row.max ?? "–"}</td>
        <td class="${row.valid ? "adc-ok" : "adc-fault"}">${row.valid === true ? "OK" : row.valid === false ? "Fehler" : "–"}</td>
      </tr>`).join("");
  }

  function calibrationPoint(point, sensor, channel, unit) {
    const calibrated = point?.calibrated === true;
    const value = unit === "A" ? point?.valueA : point?.valueV;
    const voltageAveraging = state.calibration?.voltage?.averaging || {};
    const voltageBusy = sensor === "voltage" && voltageAveraging.active === true;
    const isActiveVoltagePoint = voltageBusy && Number(voltageAveraging.point) === Number(point?.index);
    const disabled = voltageBusy ? "disabled" : "";
    const attributes = `data-calibration-sensor="${sensor}" data-calibration-point="${point?.index ?? 0}"${channel === null ? "" : ` data-calibration-channel="${channel}"`}`;
    return `
      <div class="calibration-point ${calibrated ? "is-calibrated" : ""}">
        <div class="calibration-reference"><strong>${numberOrDash(value)} ${unit}</strong><span class="calibration-status">${calibrated ? "Kalibriert" : "Nicht kalibriert"}</span></div>
        <div class="calibration-raw"><small>Gespeicherter Rohwert</small><strong>${calibrated ? point.raw : "–"}</strong></div>
        <div class="calibration-actions">
          <button class="calibration-save" type="button" ${attributes} ${disabled}>${isActiveVoltagePoint ? "Messung läuft …" : calibrated ? "Neu messen" : "Punkt messen"}</button>
          ${calibrated ? `<button class="calibration-clear" type="button" ${attributes} ${disabled}>Entfernen</button>` : ""}
        </div>
      </div>`;
  }

  function updateCalibration() {
    const calibration = state.calibration || {};
    const voltage = calibration.voltage || { liveRaw: state.measurements.batteryVoltage?.adcRaw, points: [] };
    const voltagePoints = [{ index: 0, valueV: 8 }, { index: 3, valueV: 14 }].map(({ index, valueV }) => ({
      valueV, index, ...(voltage.points?.[index] || {}),
    }));
    const voltageAveraging = voltage.averaging || { active: false, durationMs: 30000, elapsedMs: 0, sampleCount: 0 };
    const voltageProgress = clamp((voltageAveraging.elapsedMs || 0) / Math.max(1, voltageAveraging.durationMs || 30000) * 100, 0, 100);
    const voltageReference = voltageAveraging.point === 3 ? 14 : 8;
    const zeroCalibration = calibration.currentZeroCalibration || { active: false, durationMs: 30000, elapsedMs: 0, sampleCount: 0 };
    const zeroProgress = clamp((zeroCalibration.elapsedMs || 0) / Math.max(1, zeroCalibration.durationMs || 30000) * 100, 0, 100);
    const currentZeroRows = state.bikes.map((bike, channel) => {
      const current = calibration.currents?.[channel] || { liveRaw: bike.adcRaw, points: [] };
      const zeroPoint = current.points?.[0] || {};
      return `<div class="zero-channel"><strong>Bike ${channel + 1}</strong><span>Live ${current.liveRaw ?? bike.adcRaw ?? "–"}</span><span>Nullpunkt ${zeroPoint.calibrated ? zeroPoint.raw : "–"}</span></div>`;
    }).join("");
    const batteryCurrent = calibration.batteryCurrent || {
      liveRaw: state.measurements.batteryCurrent?.adcRaw, points: [],
    };
    const batteryCurrentPoints = [{ index: 0, valueA: -4 }, { index: 2, valueA: 0 }, { index: 4, valueA: 4 }].map(({ index, valueA }) => ({
      valueA, index, ...(batteryCurrent.points?.[index] || {}),
    }));

    elements.calibrationSensors.innerHTML = `
      <article class="panel calibration-card voltage-calibration">
        <div class="calibration-card-head"><div><p class="eyebrow">Spannungssensor</p><h3>Batteriespannung</h3></div><div class="live-raw"><small>Live · ${calibration.averagingWindowMs || 500} ms</small><strong>${voltage.liveRaw ?? "–"}</strong></div></div>
        <div class="calibration-points">${voltagePoints.map((point) => calibrationPoint(point, "voltage", null, "V")).join("")}</div>
        <div class="zero-progress voltage-progress" aria-label="Fortschritt Spannungsmessung"><span style="width:${voltageProgress}%"></span></div>
        <small class="zero-progress-copy">${voltageAveraging.active ? `${voltageReference} V · ${Math.ceil(((voltageAveraging.durationMs || 30000) - (voltageAveraging.elapsedMs || 0)) / 1000)} s verbleibend · ${voltageAveraging.sampleCount || 0} Messfenster` : "8 V oder 14 V einstellen und 30 Sekunden messen"}</small>
      </article>
      <article class="panel calibration-card battery-current-calibration">
        <div class="calibration-card-head"><div><p class="eyebrow">Bidirektionaler Stromsensor · GPIO32</p><h3>Batteriestrom</h3></div><div class="live-raw"><small>Live · ${calibration.averagingWindowMs || 500} ms</small><strong>${batteryCurrent.liveRaw ?? "–"}</strong></div></div>
        <div class="calibration-points">${batteryCurrentPoints.map((point) => calibrationPoint(point, "batteryCurrent", null, "A")).join("")}</div>
      </article>
      <article class="panel calibration-card bike-zero-calibration">
        <div class="calibration-card-head"><div><p class="eyebrow">Fester Faktor · ${number(calibration.currentRawPerAmp || 43.5, 1)} Digits/A</p><h3>Nullpunkte aller Bike-Sensoren</h3></div><button class="primary-button zero-calibration-start" type="button" data-current-zero-calibration ${zeroCalibration.active ? "disabled" : ""}>${zeroCalibration.active ? "Messung läuft …" : "Nullpunkte 30 s messen"}</button></div>
        <p class="zero-calibration-note">Vor dem Start sicherstellen, dass durch alle vier Bike-Sensoren 0 A fließen. Die vier Nullpunkte werden gleichzeitig über 30 Sekunden gemittelt und anschließend dauerhaft gespeichert.</p>
        <div class="zero-channels">${currentZeroRows}</div>
        <div class="zero-progress" aria-label="Fortschritt Nullpunktmessung"><span style="width:${zeroProgress}%"></span></div>
        <small class="zero-progress-copy">${zeroCalibration.active ? `${Math.ceil(((zeroCalibration.durationMs || 30000) - (zeroCalibration.elapsedMs || 0)) / 1000)} s verbleibend · ${zeroCalibration.sampleCount || 0} Messfenster` : "Bereit"}</small>
      </article>`;
  }

  function render() {
    updateOverview();
    updateBikeDetail();
    updateRanking();
    updateNavigation();
    updateNetworkInfo();
    updateAdcRawValues();
    updateCalibration();
  }

  function navigate(view) {
    const bikeMatch = view.match(/^bike-(\d)$/);
    if (bikeMatch) {
      selectedBike = Number(bikeMatch[1]);
      if (!state.bikes[selectedBike - 1]?.enabled) {
        showToast(`Bike ${selectedBike} ist deaktiviert`);
        return;
      }
      activeView = view;
    } else if (view === "settings" || view === "overview" || view === "ranking") {
      activeView = view;
    } else {
      activeView = "overview";
    }

    document.querySelectorAll(".view").forEach((section) => section.classList.remove("active"));
    document.querySelector(activeView.startsWith("bike-") ? "#view-bike" : `#view-${activeView}`).classList.add("active");
    if (activeView === "settings") updateSettings();
    updateNavigation();
    window.history.replaceState(null, "", `#${activeView}`);
    window.scrollTo({ top: 0, behavior: "smooth" });
  }

  function setMenuOpen(open) {
    elements.navigation.classList.toggle("open", open);
    elements.menuToggle.setAttribute("aria-expanded", String(open));
    elements.menuToggle.setAttribute("aria-label", open ? "Navigation schließen" : "Navigation öffnen");
  }

  function acceptTelemetry(message) {
    if (!message || message.type !== "telemetry" || !message.system || !Array.isArray(message.bikes)) return;
    state.system = { ...state.system, ...message.system };
    if (message.network) state.network = { ...state.network, ...message.network };
    if (message.measurements) state.measurements = { ...state.measurements, ...message.measurements };
    if (message.calibration) state.calibration = message.calibration;
    if (Array.isArray(message.leaderboard)) state.leaderboard = message.leaderboard;
    message.bikes.slice(0, BIKE_COUNT).forEach((incoming, index) => {
      const bike = state.bikes[index];
      bike.id = Number(incoming.id) || index + 1;
      bike.riderId = Number(incoming.riderId) || index + 1;
      bike.riderName = typeof incoming.riderName === "string" ? incoming.riderName : "";
      bike.enabled = incoming.enabled !== false;
      bike.currentA = Number.isFinite(incoming.currentA) ? incoming.currentA : null;
      bike.voltageV = Number.isFinite(incoming.voltageV) ? incoming.voltageV : null;
      bike.powerW = Number(incoming.powerW) || 0;
      bike.energyWh = Number(incoming.energyWh) || 0;
      bike.adcRaw = incoming.adcRaw;
      bike.adcRawMin = incoming.adcRawMin;
      bike.adcRawMax = incoming.adcRawMax;
      bike.measurementValid = incoming.measurementValid;
      bike.history.push(bike.enabled ? bike.powerW : 0);
      bike.history.splice(0, Math.max(0, bike.history.length - HISTORY_LENGTH));
    });
    state.lastUpdate = new Date();
    render();
  }

  function webSocketAddress() {
    const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
    return `${protocol}//${window.location.host}/ws`;
  }

  function disconnectWebSocket() {
    clearTimeout(reconnectTimer);
    reconnectTimer = null;
    if (socket) {
      socket.onclose = null;
      socket.close();
      socket = null;
    }
  }

  function connectWebSocket() {
    if (state.demoMode) return;
    disconnectWebSocket();
    setConnectionStatus("connecting", "Verbinde …");

    try {
      socket = new WebSocket(webSocketAddress());
    } catch (_error) {
      scheduleReconnect();
      return;
    }

    socket.addEventListener("open", () => {
      reconnectDelayMs = 1000;
      setConnectionStatus("live", "Live verbunden");
      socket.send(JSON.stringify({ type: "requestState" }));
    });
    socket.addEventListener("message", (event) => {
      try { acceptTelemetry(JSON.parse(event.data)); } catch (_error) { /* Ignore malformed packets. */ }
    });
    socket.addEventListener("close", () => {
      socket = null;
      setConnectionStatus("offline", "Verbindung weg");
      scheduleReconnect();
    });
    socket.addEventListener("error", () => socket?.close());
  }

  function scheduleReconnect() {
    if (state.demoMode || reconnectTimer) return;
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      connectWebSocket();
    }, reconnectDelayMs);
    reconnectDelayMs = Math.min(reconnectDelayMs * 1.7, 10000);
  }

  function demoTick() {
    const elapsedHours = DEMO_INTERVAL_MS / 3600000;
    let generatedPowerW = 0;
    state.bikes.forEach((bike) => {
      if (!bike.enabled) {
        bike.powerW = 0;
      } else {
        if (Math.random() < 0.08) bike.demoTarget = Math.random() < 0.12 ? 0 : 20 + Math.random() * 80;
        const noise = (Math.random() - 0.5) * 18;
        bike.powerW = clamp(bike.powerW + (bike.demoTarget - bike.powerW) * 0.22 + noise, 0, MAX_POWER_W);
        bike.energyWh += bike.powerW * elapsedHours;
        generatedPowerW += bike.powerW;
      }
      bike.voltageV = state.system.batteryVoltage;
      bike.currentA = bike.voltageV > 0 ? bike.powerW / bike.voltageV : 0;
      bike.history.push(bike.powerW);
      bike.history.shift();
    });

    const loadPowerW = 215 + Math.sin(Date.now() / 9000) * 8;
    const netPowerW = generatedPowerW - loadPowerW;
    state.system.loadPowerW = loadPowerW;
    state.system.batteryPercent = clamp(state.system.batteryPercent + netPowerW * elapsedHours / 3.2, 0, 100);
    state.system.batteryVoltage = 12.0 + state.system.batteryPercent * 0.014 + (Math.random() - 0.5) * 0.04;
    state.system.batteryCurrentA = netPowerW / Math.max(10, state.system.batteryVoltage);
    state.system.batteryPowerW = netPowerW;
    state.system.state = netPowerW > 8 ? "charging" : netPowerW < -8 ? "discharging" : "balanced";
    state.system.remainingMinutes = state.system.state === "discharging"
      ? Math.round((state.system.batteryPercent / 100 * 320) / Math.max(1, -netPowerW) * 60)
      : -1;
    state.system.connectedClients = 3;
    state.lastUpdate = new Date();
    render();
  }

  function startDemo() {
    disconnectWebSocket();
    clearInterval(demoTimer);
    state.system.batteryPercent = state.system.batteryPercent || 74;
    setConnectionStatus("demo", "Demo läuft");
    demoTick();
    demoTimer = setInterval(demoTick, DEMO_INTERVAL_MS);
  }

  function setDemoMode(enabled) {
    state.demoMode = enabled;
    clearInterval(demoTimer);
    demoTimer = null;
    if (enabled) startDemo(); else connectWebSocket();
    updateSettings();
  }

  function saveSettings() {
    const activeBikes = Array.from(document.querySelectorAll("[data-bike-enabled]"), (input) => input.checked);
    state.bikes.forEach((bike, index) => {
      bike.enabled = activeBikes[index];
      if (!bike.enabled) bike.powerW = 0;
    });

    const requestedDemoMode = elements.demoMode.checked;
    const wifiSsid = elements.wifiSsid.value.trim();
    const wifiPassword = elements.wifiPassword.value;
    if (!requestedDemoMode && socket?.readyState === WebSocket.OPEN) {
      const request = { type: "config", activeBikes };
      if (wifiSsid) request.wifi = { ssid: wifiSsid, password: wifiPassword };
      socket.send(JSON.stringify(request));
    }
    if (requestedDemoMode !== state.demoMode) setDemoMode(requestedDemoMode);
    render();
    showToast(state.demoMode ? "Demo-Einstellungen übernommen" :
      wifiSsid ? "WLAN gespeichert – ESP32 startet neu" : "Konfiguration an ESP32 gesendet");
  }

  function selectSettingsPane(pane) {
    if (pane === "calibration" && !calibrationPassword) {
      const enteredPassword = window.prompt("Passwort für die Kalibrierung:");
      if (enteredPassword === null) return;
      if (enteredPassword !== "pedalkino") {
        showToast("Falsches Kalibrierpasswort");
        return;
      }
      calibrationPassword = enteredPassword;
    }
    document.querySelectorAll("[data-settings-pane]").forEach((button) => {
      button.classList.toggle("active", button.dataset.settingsPane === pane);
    });
    document.querySelectorAll(".settings-pane").forEach((section) => {
      section.classList.toggle("active", section.id === `settings-${pane}`);
    });
  }

  function changeCalibration(button, calibrated) {
    if (state.demoMode || socket?.readyState !== WebSocket.OPEN) {
      showToast("Kalibrierung benötigt eine Live-Verbindung zum ESP32");
      return;
    }
    const request = {
      type: "calibration",
      sensor: button.dataset.calibrationSensor,
      point: Number(button.dataset.calibrationPoint),
      calibrated,
      password: calibrationPassword,
    };
    if (button.dataset.calibrationChannel !== undefined) {
      request.channel = Number(button.dataset.calibrationChannel);
    }
    socket.send(JSON.stringify(request));
    showToast(request.sensor === "voltage" && calibrated
      ? "30-Sekunden-Spannungsmessung gestartet"
      : calibrated ? "Kalibrierpunkt gespeichert" : "Kalibrierpunkt entfernt");
  }

  function startCurrentZeroCalibration() {
    if (state.demoMode || socket?.readyState !== WebSocket.OPEN) {
      showToast("Nullpunktmessung benötigt eine Live-Verbindung zum ESP32");
      return;
    }
    if (!window.confirm("Fließen sicher 0 A durch alle vier Bike-Sensoren?")) return;
    socket.send(JSON.stringify({ type: "currentZeroCalibration", password: calibrationPassword }));
    showToast("30-Sekunden-Nullpunktmessung gestartet");
  }

  document.querySelectorAll(".view-nav button").forEach((button) => {
    button.addEventListener("click", () => {
      navigate(button.dataset.view);
      setMenuOpen(false);
    });
  });
  elements.menuToggle.addEventListener("click", () => {
    setMenuOpen(!elements.navigation.classList.contains("open"));
  });
  document.addEventListener("click", (event) => {
    if (!elements.navigation.contains(event.target) && !elements.menuToggle.contains(event.target)) setMenuOpen(false);
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape") setMenuOpen(false);
  });
  document.querySelector("#save-settings").addEventListener("click", saveSettings);
  document.querySelector("#reset-ranking").addEventListener("click", resetRanking);
  document.querySelectorAll("[data-settings-pane]").forEach((button) => {
    button.addEventListener("click", () => selectSettingsPane(button.dataset.settingsPane));
  });
  elements.calibrationSensors.addEventListener("click", (event) => {
    const zeroButton = event.target.closest("[data-current-zero-calibration]");
    const saveButton = event.target.closest(".calibration-save");
    const clearButton = event.target.closest(".calibration-clear");
    if (zeroButton) startCurrentZeroCalibration();
    else if (saveButton) changeCalibration(saveButton, true);
    else if (clearButton) changeCalibration(clearButton, false);
  });
  window.addEventListener("hashchange", () => navigate(window.location.hash.slice(1)));

  updateSettings();
  navigate(window.location.hash.slice(1) || "overview");
  if (state.demoMode) startDemo(); else connectWebSocket();
})();
