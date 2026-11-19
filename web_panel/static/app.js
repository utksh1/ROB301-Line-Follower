/* STM32 Line Follower Web Control Cockpit Client */

let ws = null;
let reconnectTimer = null;
let currentBleId = "XRSC";
let isBleConnected = false;
let currentSpeed = 70;

// Chart Data Buffers
const MAX_CHART_POINTS = 100;
const historyError = new Array(MAX_CHART_POINTS).fill(0);
const historyLeftPwm = new Array(MAX_CHART_POINTS).fill(70);
const historyRightPwm = new Array(MAX_CHART_POINTS).fill(70);

// Canvas
let canvas, ctx;

document.addEventListener("DOMContentLoaded", () => {
  initCanvas();
  connectWebSocket();
  initKeyboardShortcuts();
});

function getWsUrl() {
  const host = window.location.hostname || "localhost";
  return `ws://${host}:8765`;
}

function connectWebSocket() {
  const wsUrl = getWsUrl();
  console.log(`[WS] Connecting to ${wsUrl}...`);

  try {
    ws = new WebSocket(wsUrl);
  } catch (err) {
    console.error("[WS] Connection failed:", err);
    scheduleReconnect();
    return;
  }

  ws.onopen = () => {
    console.log("[WS] Connected to backend!");
    clearTimeout(reconnectTimer);
    addLogLine(">> WebSocket Connected to Cockpit Server", "highlight");
  };

  ws.onmessage = (event) => {
    try {
      const msg = JSON.parse(event.data);
      handleServerMessage(msg);
    } catch (e) {
      console.error("[WS] Message parse error:", e);
    }
  };

  ws.onclose = () => {
    console.warn("[WS] Disconnected from backend.");
    setBleStatus("disconnected", currentBleId);
    scheduleReconnect();
  };

  ws.onerror = (err) => {
    console.error("[WS] Socket error:", err);
    ws.close();
  };
}

function scheduleReconnect() {
  clearTimeout(reconnectTimer);
  reconnectTimer = setTimeout(() => {
    connectWebSocket();
  }, 2000);
}

function sendWs(payload) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify(payload));
  } else {
    console.warn("[WS] Not connected, cannot send:", payload);
  }
}

function sendCommand(char) {
  sendWs({ type: "cmd", cmd: char });
}

function setBleCode(code) {
  document.getElementById("bleIdInput").value = code;
}

function selectAndConnectBot(id) {
  document.getElementById("bleIdInput").value = id;
  document.querySelectorAll(".chip-bot").forEach(c => c.classList.remove("active-bot"));
  const activeEl = document.getElementById(`chip${id}`);
  if (activeEl) activeEl.classList.add("active-bot");

  setBleStatus("connecting", id);
  sendWs({ type: "connect_ble", id: id });
}

function toggleBleConnection() {
  const idInput = document.getElementById("bleIdInput").value.trim().toUpperCase();
  if (!idInput) return;

  const btn = document.getElementById("btnConnectBle");
  btn.disabled = true;
  setTimeout(() => { btn.disabled = false; }, 1500);

  if (isBleConnected) {
    sendWs({ type: "disconnect_ble" });
  } else {
    setBleStatus("connecting", idInput);
    sendWs({ type: "connect_ble", id: idInput });
  }
}

function sendPresetSpeed(key) {
  sendWs({ type: "preset_speed", preset: key });
}

function nudgeSpeed(delta) {
  if (delta > 0) {
    sendCommand("+");
  } else {
    sendCommand("-");
  }
}

function onSliderSpeed(val) {
  const speed = parseInt(val);
  // Find nearest preset key
  const presets = [30, 35, 40, 45, 50, 55, 60, 70, 80];
  let closestIdx = 0;
  let minDiff = 999;
  for (let i = 0; i < presets.length; i++) {
    const diff = Math.abs(presets[i] - speed);
    if (diff < minDiff) {
      minDiff = diff;
      closestIdx = i;
    }
  }
  sendPresetSpeed(String(closestIdx + 1));
}

function queryPidGains() {
  sendWs({ type: "query_pid" });
}

function handleServerMessage(msg) {
  switch (msg.type) {
    case "init":
      setBleStatus(msg.ble_status, msg.ble_id);
      if (msg.ble_id) {
        document.querySelectorAll(".chip-bot").forEach(c => c.classList.remove("active-bot"));
        const activeEl = document.getElementById(`chip${msg.ble_id}`);
        if (activeEl) activeEl.classList.add("active-bot");
      }
      if (msg.pid) updatePidDisplay(msg.pid);
      if (msg.speed) updateSpeedDisplay(msg.speed);
      if (msg.state) updateRobotState(msg.state);
      break;

    case "ble_status":
      setBleStatus(msg.status, msg.id);
      break;

    case "telemetry":
      updateTelemetry(msg);
      break;

    case "pid":
      if (msg.data) updatePidDisplay(msg.data);
      if (msg.raw) addLogLine(msg.raw, "highlight");
      break;

    case "speed":
      if (msg.speed) updateSpeedDisplay(msg.speed);
      if (msg.raw) addLogLine(msg.raw, "highlight");
      break;

    case "log":
    case "bridge_log":
      addLogLine(msg.text);
      break;
  }
}

function setBleStatus(status, id) {
  const dot = document.getElementById("bleStatusDot");
  const txt = document.getElementById("bleStatusText");
  const btnTxt = document.getElementById("txtConnectBtn");
  const input = document.getElementById("bleIdInput");

  if (id) {
    currentBleId = id;
    input.value = id;
  }

  dot.className = "status-dot";

  if (status === "connected") {
    isBleConnected = true;
    dot.classList.add("connected");
    txt.textContent = `CONNECTED (${currentBleId})`;
    btnTxt.textContent = "DISCONNECT";
  } else if (status === "connecting") {
    isBleConnected = false;
    dot.classList.add("connecting");
    txt.textContent = `CONNECTING (${currentBleId})...`;
    btnTxt.textContent = "ABORT";
  } else {
    isBleConnected = false;
    txt.textContent = "DISCONNECTED";
    btnTxt.textContent = "CONNECT";
  }
}

function updateRobotState(state) {
  const badge = document.getElementById("robotStateBadge");
  badge.textContent = state;
  badge.className = "badge";

  if (state === "FOLLOW") {
    badge.classList.add("state-follow");
  } else if (state === "REVERSE") {
    badge.classList.add("state-reverse");
  } else {
    badge.classList.add("state-stopped");
  }
}

function updateSpeedDisplay(speed) {
  currentSpeed = speed;
  document.getElementById("speedValue").textContent = speed;
  document.getElementById("speedSlider").value = speed;

  // Highlight matching pill
  document.querySelectorAll(".preset-pill").forEach(p => p.classList.remove("active"));
  const pillMap = { 30: "1", 40: "3", 50: "5", 60: "7", 70: "8", 80: "9" };
  const targetText = `${speed}%`;
  document.querySelectorAll(".preset-pill").forEach(p => {
    if (p.textContent === targetText) {
      p.classList.add("active");
    }
  });
}

function updatePidDisplay(pid) {
  if (pid.kp !== undefined) document.getElementById("valKp").textContent = pid.kp.toFixed(1);
  if (pid.kd !== undefined) document.getElementById("valKd").textContent = pid.kd.toFixed(1);
  if (pid.ki !== undefined) document.getElementById("valKi").textContent = pid.ki.toFixed(2);
}

function updateTelemetry(data) {
  updateRobotState(data.state);

  // Line Validity Badge
  const valBadge = document.getElementById("lineValidityBadge");
  if (data.valid) {
    valBadge.textContent = "VALID";
    valBadge.style.background = "var(--emerald-glow)";
    valBadge.style.color = "var(--emerald)";
    valBadge.style.borderColor = "var(--emerald)";
  } else {
    valBadge.textContent = "INVALID";
    valBadge.style.background = "var(--crimson-glow)";
    valBadge.style.color = "var(--crimson)";
    valBadge.style.borderColor = "var(--crimson)";
  }

  // Position, Error, Correction
  const p = data.position !== null ? data.position : 3.5;
  const e = data.error !== null ? data.error : 0.0;
  const c = data.correction || 0;

  document.getElementById("valPosition").textContent = data.valid && data.position !== null ? p.toFixed(2) : "--";
  document.getElementById("valError").textContent = data.valid && data.error !== null ? (e >= 0 ? `+${e.toFixed(2)}` : e.toFixed(2)) : "--";
  document.getElementById("valCorrection").textContent = c >= 0 ? `+${c}` : c;

  // Analog needle position: P maps from 0.0 to 7.0 (0% to 100%)
  const needlePct = Math.max(0, Math.min(100, (p / 7.0) * 100));
  const needle = document.getElementById("trackNeedle");
  needle.style.left = `${needlePct}%`;
  if (Math.abs(e) <= 0.55) {
    needle.style.background = "var(--emerald)";
    needle.style.boxShadow = "0 0 10px var(--emerald)";
  } else if (Math.abs(e) <= 1.5) {
    needle.style.background = "var(--cyan)";
    needle.style.boxShadow = "0 0 10px var(--cyan)";
  } else {
    needle.style.background = "var(--amber)";
    needle.style.boxShadow = "0 0 10px var(--amber)";
  }

  // Update 8 IR sensor LEDs
  updateSensorLeds(p, data.valid);

  // Motor PWM Gauges
  const l = data.left_pwm || 0;
  const r = data.right_pwm || 0;
  document.getElementById("valLeftPwm").textContent = `${l}%`;
  document.getElementById("valRightPwm").textContent = `${r}%`;

  document.getElementById("barLeftPwm").style.width = `${Math.max(0, Math.min(100, Math.abs(l)))}%`;
  document.getElementById("barRightPwm").style.width = `${Math.max(0, Math.min(100, Math.abs(r)))}%`;

  // Push to chart buffers
  historyError.shift();
  historyError.push(e);

  historyLeftPwm.shift();
  historyLeftPwm.push(l);

  historyRightPwm.shift();
  historyRightPwm.push(r);

  // Log raw line
  if (data.raw) {
    addLogLine(data.raw);
  }
}

function updateSensorLeds(pos, isValid) {
  for (let i = 0; i < 8; i++) {
    const el = document.getElementById(`sens${i}`);
    if (!isValid) {
      el.classList.remove("active");
      continue;
    }
    // Sensor is active if line position is close to its index
    const dist = Math.abs(pos - i);
    if (dist <= 0.8) {
      el.classList.add("active");
    } else {
      el.classList.remove("active");
    }
  }
}

function addLogLine(text, className = "") {
  const container = document.getElementById("termLogs");
  const line = document.createElement("div");
  line.className = `term-line ${className}`;
  
  const d = new Date();
  const ts = d.toTimeString().split(" ")[0] + "." + String(d.getMilliseconds()).padStart(3, "0");
  line.textContent = `[${ts}] ${text}`;

  container.appendChild(line);

  // Keep max 200 lines
  if (container.children.length > 200) {
    container.removeChild(container.firstChild);
  }

  const autoscroll = document.getElementById("chkAutoscroll").checked;
  if (autoscroll) {
    container.scrollTop = container.scrollHeight;
  }
}

function clearConsole() {
  document.getElementById("termLogs").innerHTML = "";
}

// ── Chart Rendering (Canvas) ────────────────────────────────────────────────
function initCanvas() {
  canvas = document.getElementById("telemetryChart");
  ctx = canvas.getContext("2d");

  function resize() {
    canvas.width = canvas.parentElement.clientWidth * window.devicePixelRatio;
    canvas.height = canvas.parentElement.clientHeight * window.devicePixelRatio;
    ctx.scale(window.devicePixelRatio, window.devicePixelRatio);
  }

  window.addEventListener("resize", resize);
  resize();

  function render() {
    drawChart();
    requestAnimationFrame(render);
  }
  requestAnimationFrame(render);
}

function drawChart() {
  if (!canvas || !ctx) return;
  const w = canvas.parentElement.clientWidth;
  const h = canvas.parentElement.clientHeight;

  ctx.clearRect(0, 0, w, h);

  // Grid lines
  ctx.strokeStyle = "#1b2533";
  ctx.lineWidth = 1;

  // Center zero line
  const midY = h / 2;
  ctx.beginPath();
  ctx.setLineDash([4, 4]);
  ctx.moveTo(0, midY);
  ctx.lineTo(w, midY);
  ctx.stroke();
  ctx.setLineDash([]);

  // Plot Error Curve (amber)
  // Error range: -3.5 to +3.5 -> mapped to h to 0
  ctx.strokeStyle = "#f59e0b";
  ctx.lineWidth = 2.5;
  ctx.beginPath();
  for (let i = 0; i < MAX_CHART_POINTS; i++) {
    const x = (i / (MAX_CHART_POINTS - 1)) * w;
    const e = historyError[i];
    // Map -3.5 to +3.5 to h - 10 to 10
    const y = midY - (e / 3.5) * (midY - 14);
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  }
  ctx.stroke();

  // Plot Left Motor (cyan)
  ctx.strokeStyle = "#06b6d4";
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  for (let i = 0; i < MAX_CHART_POINTS; i++) {
    const x = (i / (MAX_CHART_POINTS - 1)) * w;
    const pwm = historyLeftPwm[i]; // 0 to 100
    const y = h - (pwm / 100) * (h - 20) - 10;
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  }
  ctx.stroke();

  // Plot Right Motor (pink)
  ctx.strokeStyle = "#ec4899";
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  for (let i = 0; i < MAX_CHART_POINTS; i++) {
    const x = (i / (MAX_CHART_POINTS - 1)) * w;
    const pwm = historyRightPwm[i];
    const y = h - (pwm / 100) * (h - 20) - 10;
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  }
  ctx.stroke();
}

// ── Keyboard Shortcuts ──────────────────────────────────────────────────────
function initKeyboardShortcuts() {
  window.addEventListener("keydown", (e) => {
    // Ignore keystrokes when typing in an input
    if (e.target.tagName === "INPUT") return;

    if (e.code === "Space") {
      e.preventDefault();
      sendCommand("x");
      addLogLine("[KEY] Space -> STOP", "highlight");
    } else if (e.code === "Enter") {
      e.preventDefault();
      sendCommand("s");
      addLogLine("[KEY] Enter -> START", "highlight");
    } else if (e.key === "r" || e.key === "R") {
      sendCommand("r");
      addLogLine("[KEY] R -> REVERSE", "highlight");
    } else if (e.key === "+" || e.key === "=") {
      nudgeSpeed(+5);
    } else if (e.key === "-" || e.key === "_") {
      nudgeSpeed(-5);
    } else if (e.key >= "1" && e.key <= "9") {
      sendPresetSpeed(e.key);
    }
  });
}

