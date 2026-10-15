// SPDX-License-Identifier: GPL-3.0-or-later
import assert from "node:assert/strict";
import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { gunzipSync } from "node:zlib";

const root = resolve(import.meta.dirname, "../..");
const moduleRoot = process.env.WICAN_WEB_TEST_MODULES;
const { JSDOM, VirtualConsole } = moduleRoot
  ? await import(pathToFileURL(resolve(moduleRoot, "jsdom/lib/api.js")))
  : await import("jsdom");
const uiRoot = process.env.MEATPI_COMPONENTS_PATH || resolve(root, "components/meatpi/components");
const minified = process.env.WICAN_PAN_MINIFIED_INPUT;
const original = minified ? gunzipSync(readFileSync(minified)).toString("utf8")
  : readFileSync(resolve(uiRoot, "web_ui_v2/web/index.html"), "utf8");
const shim = readFileSync(resolve(root, "tools/webui_preview/mock_api.js"), "utf8");
const extension = readFileSync(resolve(root, "components/usb_bluetooth_pan/web/usb-pan.js"), "utf8");
const settings = JSON.parse(readFileSync(resolve(root, "tools/webui_preview/mock_settings.json"), "utf8"));
const values = {
  enabled: true, peer_address: "AA:BB:CC:DD:EE:01", device_name: "WiCAN PRO PAN",
  auto_connect: true, prefer_pan: false, reconnect_min_s: 5, reconnect_max_s: 60,
  connect_timeout_s: 30, dhcp_timeout_s: 30, pairing_timeout_s: 120,
  allow_legacy_pairing: false, legacy_pin_password: "", cli: true
};
settings.usb_bluetooth_pan = {
  values,
  schema: { type: "object", properties: Object.fromEntries(Object.entries(values).map(([key, value]) =>
    [key, { type: typeof value === "boolean" ? "boolean" : typeof value === "number" ? "integer" : "string", default: value }])) }
};
const injection = `<script>window.__MOCK_SETTINGS__=${JSON.stringify(settings)};</script><script>${shim}</script>`;
function previewHtml(mock = "") {
  const html = original.replace(/<script\b/, () => injection + mock + "<script");
  return minified ? html : html.replace("\nboot();\n", () => "\n" + extension + "\nboot();\n");
}
const html = previewHtml();
const errors = [];
const console = new VirtualConsole();
console.on("jsdomError", error => errors.push(error.message));
const dom = new JSDOM(html, {
  runScripts: "dangerously", url: "http://wican.local/#/settings/bluetooth",
  pretendToBeVisual: true, virtualConsole: console,
  beforeParse(window) {
    window.matchMedia = () => ({ matches: false, addListener() {}, addEventListener() {} });
    window.scrollTo = () => {};
    window.HTMLCanvasElement.prototype.getContext = () => null;
    window.TextEncoder = TextEncoder;
    window.TextDecoder = TextDecoder;
  }
});
const window = dom.window;
const actions = [];
const status = {
  compiled: true, enabled: true, usb_host_enabled: true, dongle_present: true,
  state: "ready", bonded: false, numeric_pending: false, network_up: false,
  adapter_address: "00:1A:7D:DA:71:14", peer_address: values.peer_address,
  adapter_product: "CSR8510 A10", error: "", devices: []
};
function installPanMock(window) {
const status = window.__panStatus, actions = window.__panActions;
const previousFetch = window.fetch;
window.fetch = async (path, options = {}) => {
  if (path === "/api/usb/pan") return { ok: true, headers: { get: () => "application/json" }, json: async () => ({ ...status }) };
  if (path === "/api/usb/pan/action") {
    const body = JSON.parse(options.body);
    actions.push(body);
    if (body.action === "pair") { status.numeric_pending = true; status.passkey = 123456; status.state = "confirm_pairing"; }
    if (body.action === "confirm") { status.numeric_pending = false; status.bonded = true; status.network_up = true; status.state = "connected"; status.ip = "192.168.44.2"; }
    if (body.action === "disconnect") { status.network_up = false; status.state = "disconnected"; status.manual_disconnect = true; status.ip = ""; }
    if (body.action === "connect") { status.network_up = true; status.state = "connected"; status.manual_disconnect = false; status.ip = "192.168.44.2"; }
    return { ok: true, headers: { get: () => "application/json" }, json: async () => ({ ok: true }) };
  }
  return previousFetch(path, options);
};
}
window.__panStatus = status; window.__panActions = actions;
installPanMock(window);
if (process.env.WICAN_PAN_PREVIEW_OUTPUT) {
  const mock = `<script>window.__panStatus=${JSON.stringify(status)};window.__panActions=[];(${installPanMock.toString()})(window);</script>`;
  writeFileSync(process.env.WICAN_PAN_PREVIEW_OUTPUT, previewHtml(mock));
}
const wait = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
const query = selector => window.document.querySelector(selector);
try {
  await wait(1800);
  assert(query("[data-usb-pan]"), "PAN panel appears in the existing Bluetooth tab");
  assert.equal(query('[data-usb-pan] [data-key="legacy_pin_password"] input').type, "password");
  assert.match(query("[data-pan-status]").textContent, /CSR8510 A10/);
  assert.equal(query('[data-pan-action="pair"]').disabled, false);
  query('[data-pan-action="pair"]').click();
  await wait(100);
  assert.equal(query("[data-pan-code]").textContent, "123456");
  assert.equal(actions.length, 1, "Numeric confirmation is not automatically accepted");
  [...query("[data-pan-pairing]").querySelectorAll("button")][0].click();
  await wait(100);
  assert.deepEqual(actions[1], { action: "confirm", value: 123456 });
  assert.match(query("[data-pan-status]").textContent, /192\.168\.44\.2/);
  assert.equal(query('[data-pan-action="connect"]').disabled, true);
  assert.equal(query('[data-pan-action="disconnect"]').disabled, false);
  query('[data-pan-action="disconnect"]').click();
  await wait(100);
  assert.equal(actions[2].action, "disconnect");
  assert.equal(query('[data-pan-action="connect"]').disabled, false);
  query('[data-pan-action="connect"]').click();
  await wait(100);
  assert.equal(actions[3].action, "connect");
  assert.equal(query('[data-pan-action="connect"]').disabled, true);
  status.compiled = false; status.state = "unavailable";
  await wait(1600);
  assert.match(query("[data-pan-status]").textContent, /Not included in this firmware/);
  for (const control of query("[data-usb-pan]").querySelectorAll("[data-pan-action]"))
    assert.equal(control.disabled, true, "Backend-disabled firmware cannot issue live commands");
  status.compiled = true; status.state = "ready";
  status.network_up = false;
  status.devices = [{ address: "AA:BB:CC:DD:EE:02", name: '<img src=x onerror="window.injected=true">', rssi: -50 }];
  await wait(1600);
  assert.equal(query("[data-pan-devices] img"), null, "Phone names render as text");
  const select = query("[data-pan-devices] button");
  assert(select, "Discovered phone has a selection control");
  select.click();
  await wait(100);
  assert.equal(query('[data-pan-action="pair"]').disabled, true, "Staged peer changes cannot pair the old active peer");
  assert.deepEqual(errors, [], "No browser script errors");
  process.stdout.write("USB PAN UI interactions: PASS\n");
} finally { window.close(); }
