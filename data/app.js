(() => {
  "use strict";

  const BIKE_COUNT = 4;
  const HISTORY_LENGTH = 60;
  const DEMO_INTERVAL_MS = 900;

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
      enabled: true,
      powerW: 0,
      energyWh: 0,
      history: Array(HISTORY_LENGTH).fill(0),
      demoTarget: 70 + Math.random() * 80,
    })),
  };

  let activeView = "overview";
  let selectedBike = 1;
  let socket = null;
  let reconnectTimer = null;
  let reconnectDelayMs = 1000;
  let demoTimer = null;
  let toastTimer = null;

  const elements = {
    connection: document.querySelector("#connection-status"),
    connectionLabel: document.querySelector("#connection-status .status-label"),
    lastUpdate: document.querySelector("#last-update"),
    batteryRing: document.querySelector("#battery-ring"),
    batteryPercent: document.querySelector("#battery-percent"),
    batteryVoltage: document.querySelector("#battery-voltage"),
    batteryCurrent: document.querySelector("#battery-current"),
    flowBadge: document.querySelector("#flow-badge"),
    remainingTime: document.querySelector("#remaining-time"),
    generatedPower: document.querySelector("#generated-power"),
    loadPower: document.querySelector("#load-power"),
    clientCount: document.querySelector("#client-count"),
    bikeGrid: document.querySelector("#bike-grid"),
    bikeDetail: document.querySelector("#bike-detail"),
    bikeSettings: document.querySelector("#bike-settings"),
    demoMode: document.querySelector("#demo-mode"),
    toast: document.querySelector("#toast"),
  };

  function number(value, digits = 0) {
    return new Intl.NumberFormat("de-DE", {
      minimumFractionDigits: digits,
      maximumFractionDigits: digits,
    }).format(Number.isFinite(value) ? value : 0);
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
    const maximum = Math.max(250, ...history, 1);
    const points = history.map((value, index) => {
      const x = (index / Math.max(1, history.length - 1)) * width;
      const y = height - (clamp(value, 0, maximum) / maximum) * (height - 8);
      return `${x.toFixed(1)},${y.toFixed(1)}`;
    }).join(" ");
    const area = `M 0,${height} L ${points.replaceAll(" ", " L ")} L ${width},${height} Z`;
    return `<svg class="sparkline" viewBox="0 0 ${width} ${height}" preserveAspectRatio="none" role="img" aria-label="${escapeText(label)}"><path class="area" d="${area}"></path><polyline points="${points}"></polyline></svg>`;
  }

  function needleGauge(powerW, bikeId, compact = false) {
    const boundedPowerW = clamp(powerW, 0, 250);
    const angle = -120 + (boundedPowerW / 250) * 240;
    const arcPoint = (value, radius = 72) => {
      const pointAngle = (-120 + (value / 250) * 240) * Math.PI / 180;
      return {
        x: 90 + Math.sin(pointAngle) * radius,
        y: 87 - Math.cos(pointAngle) * radius,
      };
    };
    const arcPath = (startValue, endValue) => {
      const start = arcPoint(startValue);
      const end = arcPoint(endValue);
      const arcDegrees = ((endValue - startValue) / 250) * 240;
      const largeArc = arcDegrees > 180 ? 1 : 0;
      return `M ${start.x.toFixed(1)} ${start.y.toFixed(1)} A 72 72 0 ${largeArc} 1 ${end.x.toFixed(1)} ${end.y.toFixed(1)}`;
    };
    const tickValues = compact ? [0, 50, 100, 150, 200, 250] : Array.from({ length: 11 }, (_, index) => index * 25);
    const ticks = tickValues.map((tickValue) => {
      const tickAngle = -120 + (tickValue / 250) * 240;
      const radians = tickAngle * Math.PI / 180;
      const outerRadius = 72;
      const innerRadius = tickValue % 50 === 0 ? 61 : 65;
      const x1 = 90 + Math.sin(radians) * innerRadius;
      const y1 = 87 - Math.cos(radians) * innerRadius;
      const x2 = 90 + Math.sin(radians) * outerRadius;
      const y2 = 87 - Math.cos(radians) * outerRadius;
      return `<line x1="${x1.toFixed(1)}" y1="${y1.toFixed(1)}" x2="${x2.toFixed(1)}" y2="${y2.toFixed(1)}"></line>`;
    }).join("");
    const labels = compact ? "" : [0, 50, 100, 150, 200, 250].map((tickValue) => {
      const tickAngle = -120 + (tickValue / 250) * 240;
      const radians = tickAngle * Math.PI / 180;
      const x = 90 + Math.sin(radians) * 52;
      const y = 90 - Math.cos(radians) * 52;
      return `<text x="${x.toFixed(1)}" y="${y.toFixed(1)}">${tickValue}</text>`;
    }).join("");

    return `
      <svg class="needle-gauge ${compact ? "compact" : "large"}" viewBox="0 0 180 130" role="img" aria-label="Bike ${bikeId}: ${number(powerW)} Watt">
        <path class="gauge-track" d="${arcPath(0, 250)}"></path>
        <path class="gauge-zone low" d="${arcPath(0, 50)}"></path>
        <path class="gauge-zone good" d="${arcPath(50, 200)}"></path>
        <path class="gauge-zone peak" d="${arcPath(200, 250)}"></path>
        <g class="gauge-ticks">${ticks}</g>
        <g class="gauge-labels">${labels}</g>
        <g class="gauge-needle" style="transform: rotate(${angle}deg)">
          <path d="M 87 87 L 90 25 L 93 87 Z"></path>
        </g>
        <circle class="gauge-hub" cx="90" cy="87" r="8"></circle>
      </svg>`;
  }

  function flowCopy(flowState) {
    if (flowState === "charging") return "Akku wird geladen";
    if (flowState === "discharging") return "Akku wird entladen";
    return "Erzeugung und Verbrauch im Gleichgewicht";
  }

  function remainingTimeCopy(minutes) {
    if (!Number.isFinite(minutes) || minutes < 0) return "Restzeit wird berechnet";
    if (minutes < 1) return "Weniger als eine Minute Puffer";
    if (minutes === 1) return "Etwa eine Minute Puffer";
    return `Etwa ${Math.round(minutes)} Minuten Puffer`;
  }

  function updateOverview() {
    const generatedPowerW = state.bikes.reduce((sum, bike) => sum + (bike.enabled ? bike.powerW : 0), 0);
    const batteryPercent = clamp(state.system.batteryPercent, 0, 100);
    elements.batteryRing.style.setProperty("--value", batteryPercent.toFixed(1));
    elements.batteryPercent.textContent = number(batteryPercent);
    elements.batteryVoltage.textContent = number(state.system.batteryVoltage, 1);
    elements.batteryCurrent.textContent = number(state.system.batteryCurrentA, 1);
    elements.remainingTime.textContent = remainingTimeCopy(state.system.remainingMinutes);
    elements.flowBadge.dataset.state = state.system.state;
    elements.flowBadge.textContent = flowCopy(state.system.state);
    elements.generatedPower.textContent = number(generatedPowerW);
    elements.loadPower.textContent = number(state.system.loadPowerW);
    elements.clientCount.textContent = number(state.system.connectedClients);
    elements.lastUpdate.textContent = state.lastUpdate
      ? `Aktualisiert ${state.lastUpdate.toLocaleTimeString("de-DE", { hour: "2-digit", minute: "2-digit", second: "2-digit" })}`
      : "Noch keine Messwerte";

    elements.bikeGrid.innerHTML = state.bikes.map((bike) => `
      <article class="bike-card panel ${bike.enabled ? "" : "disabled"}" data-bike="${bike.id}" tabindex="${bike.enabled ? "0" : "-1"}" aria-label="Bike ${bike.id}, ${bike.enabled ? `${number(bike.powerW)} Watt` : "deaktiviert"}">
        <div class="bike-card-head"><span class="bike-number">${bike.id}</span><span class="bike-state">${bike.enabled ? "Aktiv" : "Pausiert"}</span></div>
        ${needleGauge(bike.enabled ? bike.powerW : 0, bike.id, true)}
        <div class="bike-power">${bike.enabled ? number(bike.powerW) : "–"} <small>W</small></div>
        <p class="bike-energy">${number(bike.energyWh, 1)} Wh erzeugt</p>
        ${sparkline(bike.history, `Leistungsverlauf Bike ${bike.id}`)}
      </article>
    `).join("");

    elements.bikeGrid.querySelectorAll(".bike-card:not(.disabled)").forEach((card) => {
      const open = () => navigate(`bike-${card.dataset.bike}`);
      card.addEventListener("click", open);
      card.addEventListener("keydown", (event) => {
        if (event.key === "Enter" || event.key === " ") open();
      });
    });
  }

  function updateBikeDetail() {
    const bike = state.bikes[selectedBike - 1];
    if (!bike) return;
    const effort = clamp((bike.powerW / 250) * 100, 0, 100);
    const rank = [...state.bikes]
      .filter((entry) => entry.enabled)
      .sort((left, right) => right.energyWh - left.energyWh)
      .findIndex((entry) => entry.id === bike.id) + 1;

    elements.bikeDetail.innerHTML = `
      <div class="bike-detail-header">
        <div><p class="eyebrow">Einzelansicht</p><h1>Bike ${bike.id}</h1></div>
        <span class="live-chip">● ${bike.enabled ? "Live" : "Deaktiviert"}</span>
      </div>
      <div class="detail-grid">
        <article class="power-stage panel">
          <div class="power-stage-copy"><p class="eyebrow">Aktuelle Leistung</p><div class="giant-power">${bike.enabled ? number(bike.powerW) : "–"}<small> W</small></div></div>
          ${needleGauge(bike.enabled ? bike.powerW : 0, bike.id)}
          <div><div class="effort-bar" aria-label="Leistung relativ zu 250 Watt"><span style="width:${effort}%"></span></div></div>
        </article>
        <div class="detail-side">
          <article class="detail-metric panel"><small>Erzeugte Energie</small><strong>${number(bike.energyWh, 1)} Wh</strong></article>
          <article class="detail-metric panel"><small>Aktueller Rang</small><strong>${rank > 0 ? `${rank}. Platz` : "–"}</strong></article>
        </div>
        <article class="history-panel panel"><h2>Leistung der letzten Minute</h2>${sparkline(bike.history, `Leistungsverlauf Bike ${bike.id}`)}</article>
      </div>
    `;
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

  function render() {
    updateOverview();
    updateBikeDetail();
    updateNavigation();
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
    } else if (view === "settings" || view === "overview") {
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

  function acceptTelemetry(message) {
    if (!message || message.type !== "telemetry" || !message.system || !Array.isArray(message.bikes)) return;
    state.system = { ...state.system, ...message.system };
    message.bikes.slice(0, BIKE_COUNT).forEach((incoming, index) => {
      const bike = state.bikes[index];
      bike.id = Number(incoming.id) || index + 1;
      bike.enabled = incoming.enabled !== false;
      bike.powerW = Number(incoming.powerW) || 0;
      bike.energyWh = Number(incoming.energyWh) || 0;
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
        if (Math.random() < 0.08) bike.demoTarget = Math.random() < 0.12 ? 0 : 45 + Math.random() * 175;
        const noise = (Math.random() - 0.5) * 18;
        bike.powerW = clamp(bike.powerW + (bike.demoTarget - bike.powerW) * 0.22 + noise, 0, 260);
        bike.energyWh += bike.powerW * elapsedHours;
        generatedPowerW += bike.powerW;
      }
      bike.history.push(bike.powerW);
      bike.history.shift();
    });

    const loadPowerW = 215 + Math.sin(Date.now() / 9000) * 8;
    const netPowerW = generatedPowerW - loadPowerW;
    state.system.loadPowerW = loadPowerW;
    state.system.batteryPercent = clamp(state.system.batteryPercent + netPowerW * elapsedHours / 3.2, 0, 100);
    state.system.batteryVoltage = 12.0 + state.system.batteryPercent * 0.014 + (Math.random() - 0.5) * 0.04;
    state.system.batteryCurrentA = -netPowerW / Math.max(10, state.system.batteryVoltage);
    state.system.batteryPowerW = state.system.batteryVoltage * state.system.batteryCurrentA;
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
    if (!requestedDemoMode && socket?.readyState === WebSocket.OPEN) {
      socket.send(JSON.stringify({ type: "config", activeBikes }));
    }
    if (requestedDemoMode !== state.demoMode) setDemoMode(requestedDemoMode);
    render();
    showToast(state.demoMode ? "Demo-Einstellungen übernommen" : "Konfiguration an ESP32 gesendet");
  }

  document.querySelectorAll(".view-nav button").forEach((button) => {
    button.addEventListener("click", () => navigate(button.dataset.view));
  });
  document.querySelector("#save-settings").addEventListener("click", saveSettings);
  window.addEventListener("hashchange", () => navigate(window.location.hash.slice(1)));

  updateSettings();
  navigate(window.location.hash.slice(1) || "overview");
  if (state.demoMode) startDemo(); else connectWebSocket();
})();
