import { AudioPlayer } from "./audio-player.js";
import { bindMonitorControl } from "./monitor-control.js";
import RFB from "@novnc/novnc";
import "./style.css";
import { esc, filtered, wallGrid } from "./model.js";
function readPins() {
  return readIds("vmwall-selected");
}
function readIds(key) {
  try {
    const data = JSON.parse(localStorage.getItem(key) || "[]");
    return new Set(
      Array.isArray(data)
        ? data.filter((id) => /^[0-9a-f-]{36}$/.test(id))
        : [],
    );
  } catch {
    return new Set();
  }
}
const state = {
  vms: [],
  query: "",
  rate: 10000,
  selected: readPins(),
  live: new Set(),
  audit: null,
  native: false,
  control: false,
  records: [],
  // Named VM sets, stored on the server so every browser sees the same ones.
  groups: [],
  group: localStorage.getItem("vmwall-group") || "",
  // What was ticked before a group was shown; restored when it is toggled off.
  manual: readIds("vmwall-manual"),
  // How many 16:10 monitor slots each VM's displays need on the wall.
  spans: new Map(),
  aspects: new Map(),
  sidebar: localStorage.getItem("vmwall-sidebar") !== "0",
};
const clients = new Map();
const retries = new Map();
let signature = "",
  noticeTimer;
const app = document.querySelector("#app");
async function api(url, options) {
  let response = await fetch(url, options);
  if (response.status === 401) {
    await fetch("/");
    response = await fetch(url, options);
  }
  let data;
  try {
    data = await response.json();
  } catch {
    throw Error("Server response unavailable");
  }
  if (!response.ok) throw Error(data.error || "Request failed");
  return data;
}
function notify(message, error = false) {
  const box = document.querySelector("#notice");
  box.textContent = message;
  box.classList.toggle("error", error);
  box.hidden = false;
  clearTimeout(noticeTimer);
  noticeTimer = setTimeout(() => (box.hidden = true), error ? 15000 : 6000);
}
function remember() {
  localStorage.setItem("vmwall-selected", JSON.stringify([...state.selected]));
  localStorage.setItem("vmwall-manual", JSON.stringify([...state.manual]));
  localStorage.setItem("vmwall-group", state.group);
}
function disconnect() {
  for (const timer of retries.values()) clearTimeout(timer);
  retries.clear();
  for (const client of clients.values()) client.disconnect();
  clients.clear();
}
function dropLive(key) {
  clearTimeout(retries.get(key));
  retries.delete(key);
  const client = clients.get(key);
  clients.delete(key);
  client?.disconnect();
}
const active = (vm) => ["running", "paused"].includes(vm.state);
function canLive(vm) {
  return !!vm.vnc && vm.vncLocal !== false && active(vm);
}
function liveNote(vm) {
  if (!active(vm)) return "The VM is not running";
  if (!vm.vnc) return "This VM has no VNC console configured";
  if (vm.vncLocal === false)
    return "The VNC console listens on an address that is not on this host, so it cannot be viewed here";
  return "";
}
const audioPlayers = new Map();
function syncAudioButtons() {
  for (const button of document.querySelectorAll("[data-audio]")) {
    const player=audioPlayers.get(button.dataset.audio);
    button.textContent=player ? "Mute audio" : "Audio";
    button.classList.toggle("chosen",!!player);
    button.setAttribute("aria-pressed",String(!!player));
    button.title=player ? player.status+" · click to mute" : "Listen to this VM";
  }
}
function stopAudio(id) {
  const player=audioPlayers.get(id);audioPlayers.delete(id);player?.stop();syncAudioButtons();
}
function toggleAudio(id) {
  if(audioPlayers.has(id))return stopAudio(id);
  const url=new URL("/api/vms/"+id+"/audio",location.href);url.protocol=location.protocol==="https:"?"wss:":"ws:";
  const player=new AudioPlayer(url.href,error=>{
    if(player.stopped&&audioPlayers.get(id)===player)audioPlayers.delete(id);
    syncAudioButtons();if(error)notify(error,true);
  });
  audioPlayers.set(id,player);syncAudioButtons();player.start();
}
function currentVm() {
  return state.vms.find((v) => v.id === state.audit);
}
function liveKey(id) {
  return state.audit ? "audit:" + id : "wall:" + id;
}
function connectLive(id, container) {
  const key = liveKey(id);
  if (clients.has(key) || retries.has(key) || document.hidden) return;
  const url = new URL("/api/vms/" + id + "/vnc", location.href);
  url.protocol = location.protocol === "https:" ? "wss:" : "ws:";
  const control = !!state.audit && state.control;
  url.searchParams.set("control", control ? "1" : "0");
  const client = new RFB(container, url.href, { shared: true });
  client.viewOnly = !control;
  client.scaleViewport = !state.audit || !state.native;
  client.resizeSession = false;
  client.clipViewport = false;
  client.background = "#131923";
  clients.set(key, client);
  const status = container
    .closest(".monitor,.card")
    ?.querySelector(".live-status");
  client.addEventListener("connect", () => {
    container._retries = 0;
    container._connected = true;
    if (status)
      status.textContent = control
        ? "LIVE · CONTROL ENABLED"
        : "LIVE · VIEW ONLY";
    const time = container.closest(".monitor")?.querySelector(".frame-time");
    if (time) time.textContent = "Live VNC · primary head";
  });
  client.addEventListener("disconnect", (event) => {
    if (clients.get(key) !== client) return;
    clients.delete(key);
    if (status) status.textContent = "Disconnected · reconnecting";
    if (state.audit === id && state.control) {
      state.control = false;
      const button = document.querySelector("#control");
      if (button) {
        button.textContent = "Keyboard / mouse: OFF";
        button.classList.remove("danger");
      }
    }
    // A console that never connected is not coming back; say so instead of
    // retrying forever behind a blank tile.
    if (!container._connected && (container._retries || 0) >= 2) {
      state.live.delete(id);
      notify(
        (state.vms.find((v) => v.id === id)?.name || "VM") +
          ": the console refused the live viewer, so the tile is back on previews. Another VNC client may be holding it exclusively.",
        true,
      );
      if (state.audit) render();
      else {
        syncToolbar();
        syncCards();
      }
      return;
    }
    if (
      !client._authFailed &&
      container.isConnected &&
      state.live.has(id) &&
      !document.hidden
    ) {
      const attempt = (container._retries || 0) + 1;
      container._retries = attempt;
      retries.set(
        key,
        setTimeout(
          () => {
            retries.delete(key);
            if (
              container.isConnected &&
              state.live.has(id) &&
              !document.hidden
            ) {
              container.replaceChildren();
              connectLive(id, container);
            }
          },
          Math.min(30000, 1000 * 2 ** Math.min(attempt, 5)),
        ),
      );
    } else if (status)
      status.textContent = "Disconnected · toggle live to retry";
  });
  client.addEventListener("securityfailure", () => {
    client._authFailed = true;
    notify("VNC authentication failed.", true);
  });
  client.addEventListener("credentialsrequired", () => {
    const password = prompt(
      "VNC password for " + (state.vms.find((v) => v.id === id)?.name || "VM"),
    );
    if (password !== null) client.sendCredentials({ password });
    else {
      client._authFailed = true;
      client.disconnect();
    }
  });
}
function image(url, label) {
  return `<img data-src="${url}" alt="${esc(label)}"><span class="image-error" hidden></span>`;
}
// A VM with several monitors gets a tile that many slots wide. The count
// starts from the configured heads and is corrected from the first preview,
// because a configured head can be inactive.
function spanOf(vm) {
  return state.spans.get(vm.id) ?? (active(vm) ? Math.min(vm.heads, 4) : 1);
}
function card(vm) {
  const live = state.live.has(vm.id) && canLive(vm);
  const note = liveNote(vm);
  const monitors = spanOf(vm);
  // Preview: one image with every monitor side by side. Live: VNC only carries
  // the primary monitor, so the others stay on fast per-monitor captures.
  const screen = live
    ? `<div class="pane"><div class="vnc-host" data-vnc="${vm.id}"></div></div>` +
      Array.from({ length: monitors - 1 }, (_, n) =>
        `<div class="pane">${image("/api/vms/" + vm.id + "/heads/" + (n + 1) + ".png", vm.name + " display " + (n + 2))}</div>`,
      ).join("")
    : active(vm)
      ? image("/api/vms/" + vm.id + "/preview.png", vm.name + " display previews")
      : `<div class="off-screen">${vm.state === "shut off" ? "Powered off" : esc(vm.state)}</div>`;
  return `<article class="card" data-id="${vm.id}"><header><span class="dot ${esc(vm.state)}" title="${esc(vm.state)}"></span><button class="vm-name" data-audit="${vm.id}" title="${esc(vm.name)} · ${esc(vm.state)} · open monitor controls">${esc(vm.name)}</button><span class="card-status">${monitors > 1 ? monitors + " monitors · " : ""}<span class="live-status">${live ? "Connecting…" : "preview"}</span></span><button data-audit="${vm.id}">Take Control</button><button data-audio="${vm.id}" ${active(vm) && canLive(vm) ? "" : "disabled"}>Audio</button><button data-live="${vm.id}" ${note ? `disabled title="${esc(note)}"` : `title="Live view-only VNC of the primary monitor${monitors > 1 ? "; the other monitors refresh every second" : ""}"`}>${live ? "Stop" : "Live"}</button></header><div class="card-screen ${live ? "live" : ""}">${screen}</div></article>`;
}
function option(vm) {
  const on = state.selected.has(vm.id);
  return `<label class="vm-option ${on ? "checked" : ""}" title="${esc(vm.name)} · ${esc(vm.state)}"><input type="checkbox" aria-label="Show ${esc(vm.name)} (${esc(vm.state)})" data-select="${vm.id}" ${on ? "checked" : ""}><span class="dot ${esc(vm.state)}"></span><span class="vm-option-name">${esc(vm.name)}</span></label>`;
}
const byState = (a, b) =>
  stateRank(a.state) - stateRank(b.state) || a.name.localeCompare(b.name);
function selectedVms() {
  return state.vms.filter((vm) => state.selected.has(vm.id)).sort(byState);
}
let releaseMonitorControl = null;
function render() {
  releaseMonitorControl?.();
  releaseMonitorControl = null;
  disconnect();
  const vm = currentVm();
  // One slim bar carries the tabs and the current view's controls, so every
  // other pixel of height goes to the VM displays.
  const [bar, body] = (state.audit && vm ? auditView(vm) : wallView()).split(
    "<!--body-->",
  );
  app.innerHTML = `<header class="top"><div class="brand">VM WALL</div><nav><button id="wall" class="${!state.audit ? "chosen" : ""}">Wall</button><button id="audit-tab" class="${state.audit ? "chosen" : ""}" ${!state.vms.length ? "disabled" : ""}>Take Control</button></nav><div class="bar">${bar}</div><span id="connection" title="hypervisor · LAN viewer · view only by default">${connectionText()}</span></header><div id="notice" role="status" hidden></div>${body}<dialog id="saved-dialog"><div class="dialog-top"><strong>Saved display audit</strong><button id="close-saved">Close</button></div><img id="saved-image" alt="Saved native-resolution display audit"><p id="saved-description"></p><a id="saved-download">Download PNGs + manifest</a></dialog>`;
  document.querySelector("#wall").onclick = () => {
    location.hash = "";
    state.audit = null;
    state.control = false;
    render();
  };
  document.querySelector("#audit-tab").onclick = () =>
    openAudit(
      state.audit ||
        [...state.selected].find((id) => state.vms.some((v) => v.id === id)) ||
        state.vms.find((v) => v.state === "running")?.id ||
        state.vms[0]?.id,
    );
  document
    .querySelectorAll("[data-vnc]")
    .forEach((container) => connectLive(container.dataset.vnc, container));
  if (state.audit && vm) bindAudit(vm);
  else {
    bindWall();
    syncWall();
  }
  document.querySelector("#close-saved").onclick = () =>
    document.querySelector("#saved-dialog").close();
  refreshImages();
}
function connectionText() {
  return (
    state.vms.filter((v) => v.state === "running").length +
    " running / " +
    state.vms.length
  );
}
function stateRank(status) {
  return status === "running" ? 0 : status === "paused" ? 1 : 2;
}
function wallView() {
  return `<button id="toggle-sidebar" aria-pressed="${state.sidebar}" title="${state.sidebar ? "Hide" : "Show"} the VM list">☰</button><span id="selection-count"></span><button id="save-group" title="Save the selected VMs as a named group">Save to group</button><button id="update-group" title="Replace the current group's VMs with the selected ones">Update group</button><label>Preview <select id="rate">${[
    [0, "Paused"],
    [5000, "5 seconds"],
    [10000, "10 seconds"],
    [30000, "30 seconds"],
  ]
    .map(
      ([v, t]) =>
        `<option value="${v}" ${v === state.rate ? "selected" : ""}>${t}</option>`,
    )
    .join("")}</select></label><button id="live-selected" title="Live view-only VNC of the primary display for every selected VM (up to six)">Live selected</button><button id="stop-live">Stop live</button><span id="live-count"></span><button id="capture-selected" class="primary" title="Save a native-resolution audit of every selected running VM">Capture selected</button><!--body--><main class="wall-layout ${state.sidebar ? "" : "no-sidebar"}"><div class="vm-sidebar"><input id="search" type="search" placeholder="Find a VM…" value="${esc(state.query)}" aria-label="Find a VM"><div class="vm-list"></div></div><section class="wall"></section></main>`;
}
// The wall is updated in place: cards that did not change keep their image
// and their live console, so searching or ticking a VM never blanks the rest.
function syncList() {
  const list = document.querySelector(".vm-list");
  if (!list) return;
  const rows = filtered(state.vms, { ...state, showOff: true }).sort(byState);
  const query = state.query.toLowerCase();
  const groups = state.groups
    .filter((g) => g.name.toLowerCase().includes(query))
    .map(
      (g) =>
        `<button class="group-option ${g.name === state.group ? "checked" : ""}" data-group="${esc(g.name)}" aria-pressed="${g.name === state.group}" title="${g.name === state.group ? "Showing this group · click to go back to your own selection" : "Show this group on the wall"}"><span class="group-name">${esc(g.name)}</span><span class="group-count">${g.vms.length}</span></button>`,
    )
    .join("");
  list.innerHTML =
    groups +
    (rows.map(option).join("") ||
      '<div class="empty-list">No matching VMs.</div>');
}
function syncToolbar() {
  const selected = selectedVms();
  const set = (id, text) => {
    const el = document.querySelector(id);
    if (el) el.textContent = text;
  };
  set("#selection-count", selected.length + " selected");
  set("#live-count", `Live ${state.live.size}/6`);
  const enable = (id, on) => {
    const el = document.querySelector(id);
    if (el) el.disabled = !on;
  };
  syncGroups();
  enable("#live-selected", selected.some(canLive));
  enable("#stop-live", state.live.size > 0);
  enable("#capture-selected", selected.some(active));
}
function currentGroup() {
  return state.groups.find((g) => g.name === state.group);
}
function syncGroups() {
  const save = document.querySelector("#save-group");
  if (!save) return;
  const group = currentGroup();
  const same =
    group &&
    group.vms.length === state.selected.size &&
    group.vms.every((id) => state.selected.has(id));
  save.disabled = !state.selected.size;
  const update = document.querySelector("#update-group");
  update.disabled = !group || same;
  update.textContent = group ? `Update “${group.name}”` : "Update group";
}
// Clicking a group shows exactly its VMs; clicking it again goes back to
// whatever was ticked before.
function toggleGroup(name) {
  const group = state.groups.find((g) => g.name === name);
  if (!group) return;
  if (state.group === name) {
    state.group = "";
    state.selected = new Set(state.manual);
  } else {
    if (!currentGroup()) state.manual = new Set(state.selected);
    state.group = name;
    state.selected = new Set(group.vms);
  }
  for (const id of [...state.live])
    if (!state.selected.has(id)) state.live.delete(id);
  remember();
  syncWall();
}
async function loadGroups() {
  try {
    const groups = (await api("/api/groups")).groups || [];
    if (JSON.stringify(groups) === JSON.stringify(state.groups)) return;
    state.groups = groups;
    syncList();
    syncGroups();
  } catch {
    // The inventory poll already reports a lost connection.
  }
}
async function saveGroup(name) {
  try {
    const data = await api("/api/groups/" + encodeURIComponent(name), {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ vms: [...state.selected] }),
    });
    if (!currentGroup()) state.manual = new Set(state.selected);
    state.groups = data.groups;
    state.group = name;
    remember();
    syncList();
    syncGroups();
    notify(`Group “${name}” saved with ${state.selected.size} VM${state.selected.size === 1 ? "" : "s"}.`);
  } catch (error) {
    notify(error.message, true);
  }
}
function syncCards() {
  const wall = document.querySelector(".wall");
  if (!wall) return;
  const visible = new Set(selectedVms().map(vm => vm.id));
  for (const id of audioPlayers.keys())if(!visible.has(id))stopAudio(id);
  const have = new Map(
    [...wall.querySelectorAll(".card")].map((el) => [el.dataset.id, el]),
  );
  wall.querySelector(".empty")?.remove();
  let previous = null;
  for (const vm of selectedVms()) {
    const html = card(vm);
    let el = have.get(vm.id);
    have.delete(vm.id);
    if (!el || el._html !== html) {
      const holder = document.createElement("div");
      holder.innerHTML = html;
      const next = holder.firstElementChild;
      next._html = html;
      if (el) {
        dropLive("wall:" + vm.id);
        el.replaceWith(next);
      }
      el = next;
    }
    const slot = previous ? previous.nextElementSibling : wall.firstElementChild;
    if (slot !== el) wall.insertBefore(el, slot);
    previous = el;
  }
  for (const [id, el] of have) {
    dropLive("wall:" + id);
    stopAudio(id);
    el.remove();
  }
  if (!previous)
    wall.innerHTML =
      '<div class="empty"><strong>No VMs on the wall yet</strong>Tick the VMs you want to watch in the list. Your selection is remembered in this browser.</div>';
  wall
    .querySelectorAll("[data-vnc]")
    .forEach((container) => connectLive(container.dataset.vnc, container));
  layoutWall();
  syncAudioButtons();
  refreshImages();
}
// CSS divides the available wall space between the selected VMs.
function layoutWall() {
  const wall = document.querySelector(".wall");
  if (!wall) return;
  const tiles = [...wall.querySelectorAll(".card")];
  const aspects = tiles.map(tile => {
    const vm = state.vms.find(vm => vm.id === tile.dataset.id);
    const aspect = state.aspects.get(tile.dataset.id) || (16 / 9) * (vm ? spanOf(vm) : 1);
    tile.style.setProperty("--card-aspect", aspect);
    return aspect;
  });
  const { columns, rows } = wallGrid(aspects, wall.clientWidth, wall.clientHeight);
  wall.style.setProperty("--wall-columns", columns);
  wall.style.setProperty("--wall-rows", rows);
}
function syncWall() {
  syncList();
  syncToolbar();
  syncCards();
}
function bindWall() {
  const search = document.querySelector("#search");
  search.oninput = () => {
    state.query = search.value;
    syncList();
  };
  const layout = document.querySelector(".wall-layout");
  layout.onchange = (e) => {
    const id = e.target.dataset?.select;
    if (!id) return;
    e.target.checked ? state.selected.add(id) : state.selected.delete(id);
    e.target.closest(".vm-option").classList.toggle("checked", e.target.checked);
    if (!e.target.checked) state.live.delete(id);
    remember();
    syncToolbar();
    syncCards();
  };
  layout.onclick = (e) => {
    const button = e.target.closest?.("button");
    if (!button) return;
    if (button.dataset.group) return toggleGroup(button.dataset.group);
    if (button.dataset.audit) return openAudit(button.dataset.audit);
    if (button.dataset.audio) return toggleAudio(button.dataset.audio);
    const id = button.dataset.live;
    if (!id) return;
    if (state.live.has(id)) state.live.delete(id);
    else if (state.live.size >= 6)
      return notify("Choose up to six live primary consoles.", true);
    else state.live.add(id);
    syncToolbar();
    syncCards();
  };
  document.querySelector("#toggle-sidebar").onclick = (e) => {
    state.sidebar = !state.sidebar;
    localStorage.setItem("vmwall-sidebar", state.sidebar ? "1" : "0");
    e.currentTarget.setAttribute("aria-pressed", state.sidebar);
    e.currentTarget.title = (state.sidebar ? "Hide" : "Show") + " the VM list";
    layout.classList.toggle("no-sidebar", !state.sidebar);
    // The wall just changed width: refit the tiles and fetch sharper previews.
    layoutWall();
    for (const img of document.querySelectorAll("img[data-src]")) img._next = 0;
    refreshImages();
  };
  document.querySelector("#save-group").onclick = () => {
    const name = (prompt("Save the selected VMs to group:", state.group) || "").trim();
    if (!name) return;
    const existing = state.groups.find((g) => g.name === name);
    if (
      existing &&
      name !== state.group &&
      !confirm(`Group “${name}” already exists. Replace its VMs with the selected ones?`)
    )
      return;
    saveGroup(name);
  };
  document.querySelector("#update-group").onclick = () => {
    if (currentGroup()) saveGroup(state.group);
  };
  document.querySelector("#rate").onchange = (e) => {
    state.rate = Number(e.target.value);
    for (const img of document.querySelectorAll("img[data-src]")) img._next = 0;
    refreshImages();
  };
  document.querySelector("#stop-live").onclick = () => {
    state.live.clear();
    syncToolbar();
    syncCards();
  };
  document.querySelector("#live-selected").onclick = () => {
    const ids = selectedVms()
      .filter(canLive)
      .map((v) => v.id);
    state.live = new Set(ids.slice(0, 6));
    syncToolbar();
    syncCards();
    if (ids.length > 6) notify("Showing the first six selected live consoles.");
  };
  document.querySelector("#capture-selected").onclick = async (e) => {
    const ids = selectedVms()
      .filter(active)
      .map((v) => v.id);
    if (!ids.length) return notify("Select at least one running VM first.");
    if (ids.length > 20)
      return notify("Capture up to twenty selected VMs in one batch.", true);
    e.target.disabled = true;
    let done = 0;
    for (const id of ids) {
      try {
        await api("/api/vms/" + id + "/capture", { method: "POST" });
        done++;
      } catch (error) {
        notify(error.message, true);
      }
    }
    syncToolbar();
    notify(
      `Saved ${done}/${ids.length} native-resolution audits. Open each VM’s audit history to download.`,
    );
  };
}
function auditView(vm) {
  return `<select id="audit-vm" aria-label="VM to audit">${state.vms.map((v) => `<option value="${v.id}" ${v.id === vm.id ? "selected" : ""}>${esc(v.name)} · ${esc(v.state)}</option>`).join("")}</select><button id="native">${state.native ? "Fit monitors" : "Native pixels"}</button><button id="live-primary" ${liveNote(vm) ? `disabled title="${esc(liveNote(vm))}"` : ""}>${state.live.has(vm.id) ? "Stop live primary" : "Live primary"}</button><button id="control" class="${state.control ? "danger" : ""}" ${!state.live.has(vm.id) || !canLive(vm) ? 'disabled title="Start Live primary first"' : ""}>Keyboard / mouse: ${state.control ? "ON" : "OFF"}</button><button data-audio="${vm.id}" ${canLive(vm) ? "" : "disabled"}>Audio</button><button id="capture" class="primary" ${!["running", "paused"].includes(vm.state) ? "disabled" : ""}>Save full audit</button><!--body--><main class="audit-layout"><section class="monitors ${state.native ? "native" : ""}" style="--heads:${Math.min(vm.heads, 2)}">${Array.from({ length: vm.heads }, (_, head) => `<article class="monitor"><header><strong>Display ${head + 1}</strong><span class="resolution" id="resolution-${head}">Head ${head}</span><span class="live-status">${head === 0 && state.live.has(vm.id) && canLive(vm) ? "Connecting…" : "per-head capture"}</span><span class="frame-time">Waiting for frame…</span></header><div class="screen">${head === 0 && state.live.has(vm.id) && canLive(vm) ? `<div class="vnc-host" data-vnc="${vm.id}"></div>` : image("/api/vms/" + vm.id + "/heads/" + head + ".png", vm.name + " display " + (head + 1))}</div></article>`).join("")}</section><aside class="audit-side"><h2>Saved audits</h2><p>PNG for each head, native combined image, timestamps and SHA-256 manifest.</p><div id="history"></div><div class="audit-help"><strong>Click a monitor to control</strong><p>Click either display to send mouse and keyboard input. Press Left Shift + Windows key or click outside the display to release control. Saving an audit captures heads sequentially and records each timestamp.</p><p>Configured heads may include an inactive or blank display. Nothing is inferred from a black image.</p></div></aside></main>`;
}
function bindAudit(vm) {
  for (const id of audioPlayers.keys())if(id!==vm.id)stopAudio(id);
  document.querySelector("[data-audio]").onclick=()=>toggleAudio(vm.id);
  syncAudioButtons();
  document.querySelector("#audit-vm").onchange = (e) =>
    openAudit(e.target.value);
  document.querySelector("#native").onclick = () => {
    state.native = !state.native;
    render();
  };
  document.querySelector("#live-primary").onclick = () => {
    state.control = false;
    if (!state.live.has(vm.id) && state.live.size >= 6)
      return notify("Stop another live console before adding one.", true);
    state.live.has(vm.id) ? state.live.delete(vm.id) : state.live.add(vm.id);
    render();
  };
  document.querySelector("#control").onclick = () => {
    state.control = !state.control;
    render();
  };
  releaseMonitorControl = bindMonitorControl(document.querySelector(".monitors"), document.querySelector("#control"),
    (head, events) => api("/api/vms/" + vm.id + "/input", {method:"POST",body:JSON.stringify({head,events})}),
    message => notify(message,true));
  document.querySelector("#capture").onclick = async (e) => {
    e.target.disabled = true;
    try {
      const record = await api("/api/vms/" + vm.id + "/capture", {
        method: "POST",
      });
      notify(
        `Saved ${record.displays.length} display${record.displays.length === 1 ? "" : "s"}${record.unavailable.length ? " · " + record.unavailable.length + " unavailable" : ""}.`,
      );
      await history(vm.id);
      showSaved(record);
    } catch (error) {
      notify(error.message, true);
    } finally {
      const button = document.querySelector("#capture");
      if (button) button.disabled = false;
    }
  };
  history(vm.id).catch((e) => notify(e.message, true));
}
async function history(id) {
  const data = await api("/api/audits?vm=" + id);
  if (state.audit !== id) return;
  state.records = data.audits;
  const list = document.querySelector("#history");
  list.innerHTML =
    data.audits
      .map(
        (record, index) =>
          `<button class="history-item" data-record="${index}"><img src="/api/audits/${record.id}/preview.png" loading="lazy" alt="Saved audit preview"><span>${esc(new Date(record.startedAt).toLocaleString())}<small>${record.displays.map((d) => d.width + "×" + d.height).join(" + ")}</small></span></button>`,
      )
      .join("") || '<p class="muted">No saved audits yet.</p>';
  list
    .querySelectorAll("[data-record]")
    .forEach(
      (button) =>
        (button.onclick = () =>
          showSaved(state.records[Number(button.dataset.record)])),
    );
}
function showSaved(record) {
  document.querySelector("#saved-image").src =
    "/api/audits/" + record.id + "/combined.png";
  document.querySelector("#saved-description").textContent =
    record.vm.name +
    " · " +
    new Date(record.startedAt).toLocaleString() +
    " · " +
    record.displays
      .map((d) => "Display " + (d.head + 1) + " " + d.width + "×" + d.height)
      .join(" / ");
  const link = document.querySelector("#saved-download");
  link.href = "/api/audits/" + record.id + "/bundle.zip";
  link.download = "vm-audit-" + record.id + ".zip";
  document.querySelector("#saved-dialog").showModal();
}
function openAudit(id) {
  if (!id) return;
  state.audit = id;
  state.control = false;
  location.hash = "audit/" + id;
  render();
}
function refreshImages() {
  if (document.hidden) return;
  for (const img of document.querySelectorAll("img[data-src]")) {
    // Audit feeds and the extra monitors of a live tile refresh every second.
    const delay =
      state.audit || img.closest(".card-screen.live") ? 1000 : state.rate;
    if (!delay) continue;
    const rect = img.getBoundingClientRect();
    if (
      rect.bottom < 0 ||
      rect.top > innerHeight ||
      rect.width === 0 ||
      img._loading ||
      Date.now() < (img._next || 0)
    )
      continue;
    img._loading = true;
    const url = new URL(img.dataset.src, location.href);
    url.searchParams.set("t",Date.now());
    if(url.pathname.endsWith("/preview.png"))url.searchParams.set("width",Math.ceil(rect.width*(window.devicePixelRatio||1)));
    loadFrame(img, url.href, delay);
  }
}
// The next frame is fetched and decoded off-screen and only then swapped in,
// so the tile keeps showing the previous frame instead of flashing black.
async function loadFrame(img, href, delay) {
  const error = img.parentElement?.querySelector(".image-error");
  try {
    const response = await fetch(href);
    if (!response.ok) throw Error("Display unavailable");
    const frame = URL.createObjectURL(await response.blob());
    const probe = new Image();
    probe.src = frame;
    await probe.decode();
    if (!img.isConnected) return URL.revokeObjectURL(frame);
    const previous = img._frame;
    img._frame = frame;
    img.onload = () => previous && URL.revokeObjectURL(previous);
    img.src = frame;
    if (error) error.hidden = true;
    // The combined preview tells how many monitors are really active.
    const tile = img.closest(".card");
    if (tile && img.dataset.src.endsWith("/preview.png") && probe.naturalHeight) {
      const monitors = Math.max(
        1,
        Math.min(4, Math.round(probe.naturalWidth / probe.naturalHeight / 1.6)),
      );
      state.aspects.set(tile.dataset.id, probe.naturalWidth / probe.naturalHeight);
      layoutWall();
      if (state.spans.get(tile.dataset.id) !== monitors) {
        state.spans.set(tile.dataset.id, monitors);
        syncCards();
      }
    }
    const monitor = img.closest(".monitor");
    if (monitor) {
      monitor.querySelector(".resolution").textContent =
        probe.naturalWidth + " × " + probe.naturalHeight;
      monitor.querySelector(".frame-time").textContent =
        "Received " + new Date().toLocaleTimeString();
    }
  } catch {
    if (error) {
      error.textContent = "Display unavailable · retrying";
      error.hidden = false;
    }
  } finally {
    img._loading = false;
    img._next = Date.now() + delay;
  }
}
async function inventory() {
  try {
    const data = await api("/api/vms");
    state.vms = data.vms;
    const next = JSON.stringify(
      data.vms.map(({ id, name, state, heads, vnc, vncLocal }) => ({
        id,
        name,
        state,
        heads,
        vnc,
        vncLocal,
      })),
    );
    if (next !== signature) {
      signature = next;
      if (state.audit || !document.querySelector(".wall")) render();
      else {
        syncWall();
        document.querySelector("#audit-tab").disabled = !data.vms.length;
      }
    }
    document.querySelector("#connection").textContent = connectionText();
  } catch (error) {
    const target = document.querySelector("#connection");
    if (target) target.textContent = "Disconnected · retrying";
    notify(error.message, true);
  }
}
const route = location.hash.match(/^#audit\/([0-9a-f-]{36})$/);
if (route) state.audit = route[1];
render();
inventory();
loadGroups();
setInterval(inventory, 10000);
setInterval(loadGroups, 10000);
setInterval(refreshImages, 500);
document.addEventListener("visibilitychange", () => {
  if (document.hidden) disconnect();
  else {
    document.querySelectorAll("[data-vnc]").forEach((container) => {
      container.replaceChildren();
      connectLive(container.dataset.vnc, container);
    });
    refreshImages();
  }
});
window.addEventListener("resize", layoutWall);
window.addEventListener("hashchange", () => {
  const m = location.hash.match(/^#audit\/([0-9a-f-]{36})$/);
  const id = m?.[1] || null;
  if (id !== state.audit) {
    state.audit = id;
    state.control = false;
    render();
  }
});
