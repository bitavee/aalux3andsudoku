"use strict";

const WIDTH = 800;
const HEIGHT = 480;

const canvas = document.getElementById("screen");
const ctx = canvas.getContext("2d");
const statusEl = document.getElementById("status");
const logEl = document.getElementById("log");
const rescanBtn = document.getElementById("rescan");
const orientationBtn = document.getElementById("orientation");
const deviceEl = document.getElementById("device");

const ORIENTATION_KEY = "aalu-emulator-orientation";

function applyOrientation(orientation) {
  if (orientation === "portrait") {
    deviceEl.classList.add("portrait");
    orientationBtn.textContent = "↻ Horizontal";
  } else {
    deviceEl.classList.remove("portrait");
    orientationBtn.textContent = "↻ Vertical";
  }
  localStorage.setItem(ORIENTATION_KEY, orientation);
  send({ type: "orientation", orientation });
}

function currentOrientation() {
  return deviceEl.classList.contains("portrait") ? "portrait" : "landscape";
}

orientationBtn.addEventListener("click", () => {
  const current = deviceEl.classList.contains("portrait") ? "portrait" : "landscape";
  applyOrientation(current === "portrait" ? "landscape" : "portrait");
});

applyOrientation(localStorage.getItem(ORIENTATION_KEY) || "portrait");

function log(message) {
  const time = new Date().toLocaleTimeString();
  logEl.textContent += `[${time}] ${message}\n`;
  logEl.scrollTop = logEl.scrollHeight;
}

function setStatus(state) {
  statusEl.textContent = state;
  statusEl.className = `status status-${state}`;
}

function renderFramebuffer(buffer) {
  // 1bpp packed, MSB = leftmost pixel, 0 = black, 1 = white. 800*480/8 = 48000 bytes.
  const bytes = new Uint8Array(buffer);
  const imgData = ctx.createImageData(WIDTH, HEIGHT);
  const pixels = imgData.data;
  for (let i = 0; i < bytes.length; i++) {
    const byte = bytes[i];
    const baseX = (i * 8) % WIDTH;
    const baseY = Math.floor((i * 8) / WIDTH);
    for (let b = 0; b < 8; b++) {
      const isWhite = (byte >> (7 - b)) & 1;
      const c = isWhite ? 240 : 30;
      const idx = (baseY * WIDTH + baseX + b) * 4;
      pixels[idx] = c;
      pixels[idx + 1] = c;
      pixels[idx + 2] = c;
      pixels[idx + 3] = 255;
    }
  }
  ctx.putImageData(imgData, 0, 0);
}

let ws = null;
let reconnectTimer = null;

function connect() {
  clearTimeout(reconnectTimer);
  const url = `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws`;
  ws = new WebSocket(url);
  ws.binaryType = "arraybuffer";

  ws.onopen = () => {
    setStatus("connected");
    log("Connected to emulator");
    send({ type: "orientation", orientation: currentOrientation() });
  };

  ws.onclose = () => {
    setStatus("disconnected");
    log("Disconnected — retrying in 2s");
    reconnectTimer = setTimeout(connect, 2000);
  };

  ws.onerror = () => {
    log("WebSocket error");
  };

  ws.onmessage = (event) => {
    if (typeof event.data === "string") {
      try {
        const msg = JSON.parse(event.data);
        if (msg.type === "log") log(msg.message);
        else if (msg.type === "config") log(`config: ${msg.width}x${msg.height}`);
        else log(`unknown: ${event.data}`);
      } catch (e) {
        log(`bad JSON: ${event.data}`);
      }
    } else {
      renderFramebuffer(event.data);
    }
  };
}

function send(payload) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  ws.send(JSON.stringify(payload));
}

function sendButton(name, action) {
  send({ type: "button", button: name, action });
}

document.querySelectorAll("button[data-button]").forEach((btn) => {
  const name = btn.dataset.button;
  btn.addEventListener("mousedown", () => sendButton(name, "press"));
  btn.addEventListener("mouseup", () => sendButton(name, "release"));
  btn.addEventListener("mouseleave", () => sendButton(name, "release"));
});

rescanBtn.addEventListener("click", () => send({ type: "rescan" }));

// Generic physical button names. Logical behavior (Back/OK/PageNav/etc.) is
// owned by the firmware via MappedInputManager, since it changes per screen
// and per user settings.
const KEY_MAP = {
  ArrowUp:    "SIDE_UP",
  ArrowDown:  "SIDE_DOWN",
  p:          "SIDE_POWER",
  P:          "SIDE_POWER",
  "1":        "FRONT_1",
  "2":        "FRONT_2",
  "3":        "FRONT_3",
  "4":        "FRONT_4",
};

const heldKeys = new Set();
window.addEventListener("keydown", (e) => {
  const name = KEY_MAP[e.key];
  if (!name) return;
  e.preventDefault();
  if (heldKeys.has(e.key)) return;
  heldKeys.add(e.key);
  sendButton(name, "press");
});
window.addEventListener("keyup", (e) => {
  const name = KEY_MAP[e.key];
  if (!name) return;
  e.preventDefault();
  heldKeys.delete(e.key);
  sendButton(name, "release");
});

setStatus("disconnected");
connect();
