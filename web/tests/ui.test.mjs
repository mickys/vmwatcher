// Renders the real app in a simulated browser against a scripted API.
import test from "node:test";
import assert from "node:assert/strict";
import { GlobalRegistrator } from "@happy-dom/global-registrator";

GlobalRegistrator.register({ url: "http://127.0.0.1:6088/" });
globalThis.IS_REACT_ACT_ENVIRONMENT = true;
globalThis.ResizeObserver = class {
  constructor(callback) {
    this.callback = callback;
  }
  observe() {
    this.callback([]);
  }
  disconnect() {}
};
URL.createObjectURL = () => "blob:frame";
URL.revokeObjectURL = () => {};

const id = (n) => `0b5f1c4e-1111-4222-8333-44445555666${n}`;
const vm = (host, n, name, state, extra = {}) => ({
  host, id: id(n), key: host + "/" + id(n), name, state, heads: 1, vnc: true, live: true, liveNote: "", displays: [], ...extra,
});
const server = {
  setupRequired: true,
  authenticated: false,
  calls: [],
  hosts: [
    { id: "local", name: "hypervisor", type: "local", enabled: true, ok: true, error: "", count: 2 },
    { id: "hv2", name: "Rack B", type: "agent", enabled: true, ok: false, error: "Agent unreachable: timed out", count: 0 },
  ],
  vms: [vm("local", 1, "alpha", "running"), vm("local", 2, "beta", "shut off"), vm("hv2", 3, "gamma", "running", { live: false, liveNote: "needs the agent" })],
  groups: [{ name: "Night shift", vms: ["local/" + id(1)] }],
  settings: { videoEncoder: "vnc", siteName: "Lab wall", sessionHours: 12, previewSeconds: 10, maxLivePerSession: 6, maxLiveTotal: 24, controlEnabled: true, allowedHosts: [] },
};
const json = (body, status = 200) => new Response(JSON.stringify(body), { status, headers: { "Content-Type": "application/json" } });
globalThis.fetch = async (input, init = {}) => {
  const url = new URL(String(input instanceof URL ? input.href : input), location.href);
  const method = init.method || "GET";
  const body = init.body ? JSON.parse(init.body) : undefined;
  server.calls.push({ method, path: url.pathname, body });
  if (url.pathname === "/api/session")
    return json({ setupRequired: server.setupRequired, authenticated: server.authenticated, siteName: server.settings.siteName, version: "t",
                  ...(server.authenticated ? { settings: server.settings } : {}) });
  if (url.pathname === "/api/setup") {
    server.setupRequired = false;
    server.authenticated = true;
    return json({ ok: true });
  }
  if (!server.authenticated) return json({ error: "Sign in first" }, 401);
  if (url.pathname === "/api/logout") {
    server.authenticated = false;
    return json({ ok: true });
  }
  if (url.pathname === "/api/vms") return json({ hosts: server.hosts, vms: server.vms });
  if (url.pathname === "/api/groups") return json({ groups: server.groups });
  if (url.pathname === "/api/audits") return json({ audits: [] });
  if (url.pathname === "/api/settings")
    return json({
      settings: server.settings,
      videoEncoders: { vnc: "VNC", "software-h264": "Software H.264" },
      hosts: [
        { id: "local", name: "hypervisor", type: "local", enabled: true, url: "", uri: "", hasToken: false },
        { id: "hv2", name: "Rack B", type: "agent", enabled: true, url: "http://10.0.0.2:6089", uri: "", hasToken: true },
      ],
      libvirt: { available: true, problem: "" },
      server: { version: "t", hostname: "hypervisor", bind: "192.0.2.3", port: 6088, demo: false },
    });
  if (url.pathname === "/api/hosts/test") return json({ ok: true, vms: 7, running: 3, info: {} });
  if (url.pathname === "/api/hosts" && method === "POST") return json({ id: "new" }, 201);
  if (url.pathname.endsWith(".png")) return new Response(new Blob(["png"]), { status: 200 });
  return json({ error: "Not found" }, 404);
};
// happy-dom images never decode; report a dual-monitor preview.
Object.defineProperty(Image.prototype, "decode", { value: async () => {} });
Object.defineProperty(Image.prototype, "naturalWidth", { get: () => 2312 });
Object.defineProperty(Image.prototype, "naturalHeight", { get: () => 800 });
HTMLElement.prototype.getBoundingClientRect = () => ({ width: 640, height: 360, top: 0, bottom: 360, left: 0, right: 640 });

const { act, createElement } = await import("react");
const { createRoot } = await import("react-dom/client");
const { App } = await import("../src/App.jsx");

const settle = () => act(async () => { for (let i = 0; i < 6; i++) await new Promise((resolve) => setTimeout(resolve, 5)); });
const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => [...document.querySelectorAll(selector)];
const byText = (selector, text) => $$(selector).find((element) => element.textContent.trim().startsWith(text));
const type = (input, value) =>
  act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value").set.call(input, value);
    input.dispatchEvent(new Event("input", { bubbles: true }));
  });
const click = (element) => act(async () => element.click());
const go = async (hash) => {
  await act(async () => {
    location.hash = hash;
    window.dispatchEvent(new Event("hashchange"));
  });
  await settle();
};

test("first run, wall, control and settings", async () => {
  document.body.innerHTML = '<div id="app"></div>';
  const root = createRoot($("#app"));
  await act(async () => root.render(createElement(App)));
  await settle();

  // First run asks for the shared password, twice.
  assert.match($(".gate-card").textContent, /Choose the password/);
  const [first, second] = $$("input[type=password]");
  await type(first, "demo-password");
  await type(second, "different");
  await act(async () => $("form").dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })));
  assert.match($(".form-error").textContent, /do not match/);
  assert.ok(!server.calls.some((call) => call.path === "/api/setup"));
  await type(second, "demo-password");
  await act(async () => $("form").dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })));
  await settle();
  assert.deepEqual(server.calls.find((call) => call.path === "/api/setup").body, { password: "demo-password" });

  // Wall: hosts are listed separately, an unreachable one says why.
  assert.equal($(".brand").textContent, "VM Watcher");
  assert.match($("#connection").textContent, /2 running \/ 3 · 2 hosts · 1 unreachable/);
  assert.deepEqual($$(".host-name").map((element) => element.textContent), ["hypervisor", "Rack B"]);
  assert.match($(".host-error").textContent, /Agent unreachable/);
  assert.match($(".empty").textContent, /No VMs on the wall yet/);

  // Ticking VMs puts tiles on the wall; powered-off VMs show no image.
  for (const box of $$(".vm-option input")) await click(box);
  await settle();
  assert.equal($$(".card").length, 3);
  assert.equal($("#selection-count").textContent, "3 selected");
  await click(byText("button", "Hide sidebar"));
  assert.ok($(".wall-layout.no-sidebar"));
  assert.equal($("#toggle-sidebar").getAttribute("aria-expanded"), "false");
  assert.equal(localStorage.getItem("vmw-sidebar"), "0");
  assert.equal($$(".card").length, 3);
  await click(byText("button", "Show sidebar"));
  assert.equal($("#toggle-sidebar").getAttribute("aria-expanded"), "true");
  const alpha = $$(".card").find((card) => card.textContent.includes("alpha"));
  assert.match(alpha.querySelector("img").getAttribute("alt"), /alpha display previews/);
  assert.ok(server.calls.some((call) => call.path === `/api/hosts/local/vms/${id(1)}/preview.png`));
  assert.match(alpha.querySelector(".card-status").textContent, /hypervisor · 2 monitors · preview/);
  assert.match($$(".card").find((card) => card.textContent.includes("beta")).textContent, /Powered off/);
  const gamma = $$(".card").find((card) => card.textContent.includes("gamma"));
  const liveButton = [...gamma.querySelectorAll("button")].find((button) => button.textContent === "Live");
  assert.ok(liveButton.disabled && liveButton.title === "needs the agent");
  assert.equal(JSON.parse(localStorage.getItem("vmw-selected")).length, 3);
  const orderedNames = () => $$(".card .vm-name").map((label) => label.textContent);
  assert.deepEqual(orderedNames(), ["alpha", "gamma", "beta"]);
  await act(async () => gamma.querySelector(".card-screen").dispatchEvent(
    new KeyboardEvent("keydown", { key: "ArrowLeft", bubbles: true }),
  ));
  assert.deepEqual(orderedNames(), ["gamma", "alpha", "beta"]);
  const wall = $(".wall");
  const pointer = (element, type, x, y) => act(async () => element.dispatchEvent(new PointerEvent(type, {
    bubbles: true, isPrimary: true, pointerId: 1, button: 0, clientX: x, clientY: y,
  })));
  wall.setPointerCapture = () => {};
  const hitTest = document.elementFromPoint;
  document.elementFromPoint = () => alpha;
  await pointer(gamma.querySelector(".card-screen"), "pointerdown", 10, 10);
  await pointer(wall, "pointermove", 80, 10);
  assert.deepEqual(orderedNames(), ["alpha", "gamma", "beta"]);
  await pointer(wall, "pointerup", 80, 10);
  document.elementFromPoint = hitTest;
  assert.equal(alpha.querySelectorAll(".tile-resize").length, 8);
  const northWest = alpha.querySelector(".resize-nw");
  northWest.setPointerCapture = () => {};
  northWest.hasPointerCapture = () => true;
  await pointer(northWest, "pointerdown", 100, 100);
  await pointer(northWest, "pointermove", 60, 70);
  assert.ok($(".wall.custom-size"));
  assert.equal(alpha.style.width, "680px");
  assert.equal(alpha.style.height, "390px");
  assert.equal(JSON.parse(localStorage.getItem("vmw-layouts"))[""].sizes, undefined);
  await pointer(northWest, "pointerup", 60, 70);
  assert.deepEqual(JSON.parse(localStorage.getItem("vmw-layouts"))[""].sizes[`local/${id(1)}`], { width: 680, height: 390 });

  // A group replaces the selection, and toggling it off restores it.
  await click($(".group-option"));
  assert.equal($$(".card").length, 1);
  assert.match(byText(".bar button", "Update").textContent, /Night shift/);
  await click($(".group-option"));
  assert.equal($$(".card").length, 3);
  assert.deepEqual(orderedNames(), ["alpha", "gamma", "beta"]);
  assert.ok($(".wall.custom-size"));

  const fitColumns = $("#wall-fit-columns");
  assert.equal(fitColumns.value, "");
  assert.equal(fitColumns.selectedOptions[0].textContent, "Custom sizes");
  await act(async () => {
    fitColumns.value = "0";
    fitColumns.dispatchEvent(new Event("change", { bubbles: true }));
  });
  assert.ok($(".wall.fit-width"));
  assert.equal(alpha.style.width, "");
  assert.equal(alpha.style.height, "");
  assert.deepEqual(orderedNames(), ["alpha", "gamma", "beta"]);
  assert.equal(fitColumns.value, "0");
  await act(async () => {
    fitColumns.value = "2";
    fitColumns.dispatchEvent(new Event("change", { bubbles: true }));
  });
  assert.equal(wall.style.getPropertyValue("--wall-columns"), "2");
  assert.ok($(".wall.fit-width"));
  assert.equal(wall.style.getPropertyValue("--wall-rows"), "2");
  assert.equal(JSON.parse(localStorage.getItem("vmw-layouts"))[""].fit, 2);
  await pointer(northWest, "pointerdown", 100, 100);
  await pointer(northWest, "pointermove", 60, 70);
  assert.ok($(".wall.fit-width"));
  assert.equal(wall.style.getPropertyValue("--wall-columns"), "2");
  await pointer(northWest, "pointerup", 60, 70);
  assert.ok($(".wall.fit-width"));
  assert.equal(fitColumns.value, "2");
  assert.equal(alpha.style.width, "680px");
  assert.equal($$('.card').find((card) => card.querySelector(".vm-name").textContent === "gamma").style.width, "");
  await click(byText("button", "Reset sizes"));
  assert.equal(fitColumns.value, "");
  assert.equal(fitColumns.selectedOptions[0].textContent, "Default sizes");
  assert.equal($(".wall.fit-width, .wall.custom-size"), null);
  assert.equal(JSON.parse(localStorage.getItem("vmw-layouts"))[""].sizes, undefined);
  assert.deepEqual(orderedNames(), ["alpha", "gamma", "beta"]);

  assert.ok($(".header-actions #capture-selected"));

  const preview = $(".bar label select");
  assert.equal(preview.value, "10000");
  await act(async () => {
    preview.value = "2000";
    preview.dispatchEvent(new Event("change", { bubbles: true }));
  });
  await type($("#vm-sidebar input[type=search]"), "alpha");
  const hidden = Object.getOwnPropertyDescriptor(document, "hidden");
  Object.defineProperty(document, "hidden", { configurable: true, value: true });
  await click(byText("button", "Live selected"));
  assert.match($("#live-count").textContent, /Live 1\/6/);
  await click(byText("button", "Sign out"));
  assert.match($(".gate-card").textContent, /Enter the viewer password/);
  server.authenticated = true;
  await act(async () => root.render(createElement(App, { key: "signed-in-again" })));
  await settle();
  assert.equal($(".bar label select").value, "2000");
  assert.equal($("#vm-sidebar input[type=search]").value, "alpha");
  assert.match($("#live-count").textContent, /Live 1\/6/);
  await click(byText("button", "Stop live"));
  if (hidden) Object.defineProperty(document, "hidden", hidden);
  else delete document.hidden;

  // Double-click opens the full-size display without binding keyboard or mouse.
  const screen = $(".card .card-screen");
  await act(async () => screen.dispatchEvent(new MouseEvent("dblclick", { bubbles: true })));
  await settle();
  assert.equal(location.hash, "#/view/local/" + id(1));
  assert.equal($$(".monitor").length, 1);
  assert.match($(".audit-help").textContent, /View only/);
  assert.equal($(".monitor .screen").getAttribute("tabindex"), null);
  assert.equal(byText(".bar a", "Take Control").getAttribute("href"), "#/control/local/" + id(1));
  const wheel = (media, deltaY) => {
    const event = new WheelEvent("wheel", { deltaY, bubbles: true, cancelable: true });
    Object.defineProperties(event, { clientX: { value: 100 }, clientY: { value: 50 } });
    return media.dispatchEvent(event);
  };
  const previewImage = $(".monitor .screen img");
  assert.equal(wheel(previewImage, -100), false);
  assert.match(previewImage.style.transform, /scale\(1\.22/);
  Object.defineProperty(document, "hidden", { configurable: true, value: true });
  await click(byText(".bar button", "Live primary"));
  const liveScreen = $(".monitor .screen");
  const canvas = document.createElement("canvas");
  liveScreen.querySelector(".vnc-host").append(canvas);
  assert.equal(wheel(canvas, -100), false);
  assert.match(canvas.style.transform, /translate\(-22\..+px, -11\..+px\) scale\(1\.22/);
  liveScreen.classList.add("controlling");
  const zoomed = canvas.style.transform;
  assert.equal(wheel(canvas, -100), true);
  assert.equal(canvas.style.transform, zoomed);
  liveScreen.classList.remove("controlling");
  wheel(canvas, 200);
  assert.equal(canvas.style.transform, "");
  await click(byText(".bar button", "Stop live primary"));
  if (hidden) Object.defineProperty(document, "hidden", hidden);
  else delete document.hidden;


  // Take Control shows each monitor and arms the control button.
  await go("#/control/local/" + id(1));
  assert.equal($$(".monitor").length, 1);
  assert.ok($(".header-actions #capture"));
  assert.equal($("#audit-vm").value, "local/" + id(1));
  assert.equal($$("#audit-vm optgroup").length, 2);
  assert.match(byText(".bar button", "Click a monitor").textContent, /Click a monitor to control/);
  assert.match($("#history").textContent, /No saved audits yet/);

  // Settings: hosts with their status, and the add-host form.
  await go("#/settings");
  assert.match(document.body.textContent, /Software H.264/);
  const rows = $$("table.hosts tbody tr");
  assert.equal(rows.length, 2);
  assert.match(rows[0].textContent, /hypervisor.*This machine.*Connected · 2 VMs/);
  assert.match(rows[1].textContent, /Rack B.*Agent.*http:\/\/10\.0\.0\.2:6089.*Agent unreachable/);
  assert.ok(!document.body.textContent.includes("token-secret"));
  await click(byText("button", "Add host"));
  assert.match($(".host-form").textContent, /How to install the agent/);
  const inputs = $$(".host-form input");
  await type(inputs[0], "Rack C");
  await type(inputs[1], "http://10.0.0.3:6089");
  await type(inputs[2], "a".repeat(64));
  await click(byText(".host-form button", "Test connection"));
  await settle();
  assert.match($(".host-form .status-ok").textContent, /Connected: 7 VMs, 3 running/);
  await act(async () => $(".host-form").dispatchEvent(new Event("submit", { bubbles: true, cancelable: true })));
  await settle();
  const saved = server.calls.find((call) => call.path === "/api/hosts" && call.method === "POST");
  assert.deepEqual(
    { name: saved.body.name, type: saved.body.type, url: saved.body.url, token: saved.body.token },
    { name: "Rack C", type: "agent", url: "http://10.0.0.3:6089", token: "a".repeat(64) },
  );
  assert.ok(!$(".host-form"));

  // An expired session drops back to the sign-in page.
  await go("#/");
  server.authenticated = false;
  await go("#/settings"); // the next API call notices the session is gone
  assert.match($(".gate-card").textContent, /Enter the viewer password/);
  await act(async () => root.unmount());
});

test.after(() => GlobalRegistrator.unregister());
