// SPDX-License-Identifier: GPL-3.0-or-later
// Loaded before app boot; the extension uses the upstream settings store,
// controls, tabs, icons and modal helpers without modifying meatpi-components.
(() => {
  NICE.usb_bluetooth_pan = "USB Bluetooth PAN";
  Object.assign(LABELS, {
    "usb_bluetooth_pan.peer_address": "Phone Bluetooth address",
    "usb_bluetooth_pan.device_name": "Bluetooth device name",
    "usb_bluetooth_pan.auto_connect": "Automatically reconnect",
    "usb_bluetooth_pan.prefer_pan": "Prefer phone Internet over WiFi",
    "usb_bluetooth_pan.allow_legacy_pairing": "Allow legacy PIN pairing",
    "usb_bluetooth_pan.legacy_pin_password": "Legacy pairing PIN",
    "usb_bluetooth_pan.reconnect_min_s": "Initial reconnect delay (s)",
    "usb_bluetooth_pan.reconnect_max_s": "Maximum reconnect delay (s)",
    "usb_bluetooth_pan.connect_timeout_s": "Connection timeout (s)",
    "usb_bluetooth_pan.dhcp_timeout_s": "DHCP timeout (s)",
    "usb_bluetooth_pan.pairing_timeout_s": "Pairing window (s)"
  });
  const originalSettings = PAGES.settings;
  PAGES.settings = async (view, sub) => {
    const originalCleanup = await originalSettings(view, sub);
    const tab = view.querySelector('.subtab[data-id="bluetooth"]');
    if (!tab) return originalCleanup;
    const strip = tab.parentElement;
    const index = [...strip.children].indexOf(tab);
    const pane = strip.nextElementSibling.children[index];
    const root = h("div", { "data-usb-pan": "", class: "pane" });
    pane.append(root);
    let disposed = false, polling = false, busy = false, status = null;
    const timers = [];
    const close = () => {
      disposed = true;
      timers.forEach(clearInterval);
      if (originalCleanup) originalCleanup();
    };
    try {
      const values = await api("/api/settings/usb_bluetooth_pan");
      delete values.degraded; delete values.pending_reboot;
      const rows = await settingsForm("usb_bluetooth_pan", null, {
        values,
        advanced: ["device_name", "reconnect_min_s", "reconnect_max_s",
          "connect_timeout_s", "dhcp_timeout_s", "pairing_timeout_s",
          "allow_legacy_pairing", "legacy_pin_password", "cli"],
        visible: s => s.allow_legacy_pairing ? null
          : Object.keys(s).filter(key => key !== "legacy_pin_password")
      });
      const live = h("div", { "aria-live": "polite", "data-pan-status": "" });
      const devices = h("div", { "data-pan-devices": "" });
      const pairing = h("div", { "data-pan-pairing": "", hidden: true });
      const error = h("div", { "data-pan-error": "", hidden: true });
      const controls = [];
      const command = async (action, value) => {
        if (busy) return;
        busy = true; paint();
        try {
          await api("/api/usb/pan/action", { method: "POST", body: { action, ...(value === undefined ? {} : { value }) } });
          await poll();
        } catch (e) { toast(e.message, "crit"); }
        finally { busy = false; paint(); }
      };
      const button = (action, icon, label, extra) => {
        const element = h("button", {
          class: "btn sm", type: "button", title: label,
          "data-pan-action": action,
          onclick: async () => {
            if (extra && !(await extra())) return;
            await command(action);
          }
        }, ic(icon), label);
        controls.push(element);
        return element;
      };
      const scan = button("scan", "refresh", "Scan phones");
      const pair = button("pair", "key", "Pair phone");
      const connect = button("connect", "play", "Connect");
      const disconnect = button("disconnect", "pause", "Disconnect");
      const forget = button("forget", "trash", "Forget phone", () => confirmModal("Forget the saved phone bond?", "Forget"));
      const section = cardTools("USB Bluetooth PAN", "bt", [scan, pair, connect, disconnect, forget],
        live, pairing, error, ...rows, devices);
      section.querySelector(".legend").style.flexWrap = "wrap";
      Object.assign(section.querySelector(".tools").style, {
        flexWrap: "wrap", flex: "1 1 420px", minWidth: "0", maxWidth: "100%", justifyContent: "flex-start"
      });
      root.append(section);
      const labels = {
        unavailable: "Not included in this firmware", off: "Off",
        usb_disabled: "USB host disabled", waiting_for_dongle: "Waiting for USB adapter",
        initializing: "Initializing adapter", ready: "Ready", no_phone: "No phone selected", unpaired: "Phone not paired",
        scanning: "Scanning", discovering_service: "Discovering phone tethering service",
        connecting: "Connecting", confirm_pairing: "Pairing confirmation needed",
        pairing_failed: "Pairing failed", dhcp: "Obtaining IP address", connected: "Connected",
        retry_wait: "Waiting to reconnect", disconnected: "Disconnected", busy: "Busy", error: "Startup failed"
      };
      let scanSignature = "", confirmationSignature = "";
      function paint() {
        if (disposed || !status) return;
        live.replaceChildren(kv([
          ["Status", labels[status.state] || status.state],
          ["Adapter", status.adapter_product || "-"],
          ["Adapter address", status.adapter_address || "-"],
          ["Phone", status.peer_address || "-"],
          ["Bond saved", status.bonded ? "Yes" : "No"],
          ["IP address", status.ip || "-"],
          ["Gateway", status.gateway || "-"],
          ["DNS", status.dns || "-"],
          ["Reconnect", status.retry_in_s ? status.retry_in_s + " s" : "-"],
          ["Frames sent / received", (status.tx_frames || 0) + " / " + (status.rx_frames || 0)],
          ["Frames dropped", (status.tx_dropped || 0) + " / " + (status.rx_dropped || 0)]
        ]));
        const activePeer = String(status.peer_address || "").toUpperCase();
        const editedPeer = String(values.peer_address || "").toUpperCase();
        const staged = store.pending.has("usb_bluetooth_pan");
        const canCommand = status.compiled && status.enabled && status.usb_host_enabled
          && status.dongle_present && !staged && editedPeer === activePeer && !busy;
        controls.forEach(control => { control.disabled = !canCommand; });
        scan.disabled ||= status.network_up || status.state === "scanning";
        connect.disabled ||= status.network_up || !status.bonded;
        pair.disabled ||= status.network_up || status.numeric_pending || !activePeer;
        disconnect.disabled ||= !status.network_up && !["connecting", "dhcp", "discovering_service", "confirm_pairing"].includes(status.state);
        forget.disabled ||= !status.bonded;
        error.hidden = !status.error;
        error.replaceChildren(status.error ? banner("warn", "alert", status.error) : h("span"));
        const confirmationKey = status.numeric_pending ? String(status.passkey) : "";
        if (confirmationKey !== confirmationSignature) {
          confirmationSignature = confirmationKey;
          pairing.hidden = !status.numeric_pending;
          pairing.replaceChildren();
          if (status.numeric_pending) {
            const code = String(status.passkey).padStart(6, "0");
            const notice = h("div", { class: "note", role: "status" }, ic("key"),
              h("span", { class: "mono", "data-pan-code": "" }, code),
              h("button", { class: "btn sm pri", title: "Confirm matching pairing code", disabled: busy,
                onclick: () => command("confirm", status.passkey) }, ic("key"), "Confirm code"),
              h("button", { class: "btn sm", title: "Reject pairing", disabled: busy,
                onclick: () => command("reject") }, ic("pause"), "Reject"));
            notice.style.flexWrap = "wrap";
            pairing.append(notice);
          }
        }
        pairing.querySelectorAll("button").forEach(control => { control.disabled = busy; });
        const deviceSignature = JSON.stringify(status.devices || []);
        if (deviceSignature !== scanSignature) {
          scanSignature = deviceSignature;
          devices.replaceChildren();
          if (status.devices && status.devices.length) {
            devices.append(dataTable([
              { label: "Phone", get: device => device.name || device.address },
              { label: "Address", mono: true, get: device => device.address },
              { label: "Signal", get: device => device.rssi ? device.rssi + " dBm" : "-" },
              { label: "", get: device => h("button", {
                class: "btn sm", title: "Select this phone", onclick: () => {
                  values.peer_address = device.address;
                  rows.load(); store.stage("usb_bluetooth_pan", values); paint();
                }
              }, ic("phone"), "Select") }
            ], status.devices));
          }
        }
      }
      async function poll() {
        if (polling || disposed) return;
        polling = true;
        try { status = await api("/api/usb/pan"); paint(); }
        catch (e) {
          error.hidden = false;
          error.replaceChildren(banner("warn", "alert", e.message));
          controls.forEach(control => { control.disabled = true; });
        } finally { polling = false; }
      }
      await poll();
      timers.push(setInterval(poll, 1500));
    } catch (e) { root.append(banner("warn", "alert", e.message)); }
    return close;
  };
})();
