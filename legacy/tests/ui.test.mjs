import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs/promises";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { Window } from "happy-dom";
test("audit renders both heads, stays view-only until explicit control and supports pinning", async () => {
  const win = new Window({ url: "http://localhost:6088" });
  win.document.body.innerHTML = '<div id="app"></div>';
  const id = "12345678-1234-1234-1234-123456789abc";
  const instances = [];
  class MockRFB {
    constructor(container, url) {
      this.url = url;
      instances.push(this);
    }
    addEventListener() {}
    disconnect() {}
  }
  const names = [
    "window",
    "document",
    "location",
    "localStorage",
    "innerHeight",
    "fetch",
    "setInterval",
    "__TestRFB",
  ];
  const saved = new Map(names.map((n) => [n, globalThis[n]]));
  Object.assign(globalThis, {
    window: win,
    document: win.document,
    location: win.location,
    localStorage: win.localStorage,
    innerHeight: 1000,
    __TestRFB: MockRFB,
    setInterval: () => 0,
    fetch: async (url) => ({
      ok: true,
      json: async () =>
        String(url).includes("/api/vms")
          ? {
              vms: [
                {
                  id,
                  name: "<Lab & two monitors>",
                  state: "running",
                  heads: 2,
                  vnc: true,
                },
              ],
            }
          : { audits: [] },
    }),
  });
  const root = path.resolve(".work/tests");
  await fs.mkdir(root, { recursive: true });
  const target = path.join(root, "ui-under-test.mjs");
  let source = await fs.readFile("src/main.js", "utf8");
  source = source
    .replace(/["']\.\/audio-player\.js["']/g, JSON.stringify(pathToFileURL(path.resolve("src/audio-player.js")).href))
    .replace(/["']\.\/monitor-control\.js["']/g, JSON.stringify(pathToFileURL(path.resolve("src/monitor-control.js")).href))
    .replace(
      /import RFB from ["']@novnc\/novnc["'];?/g,
      "const RFB=globalThis.__TestRFB;",
    )
    .replace(/import ["']\.\/style\.css["'];?/g, "")
    .replace(
      /["']\.\/model\.js["']/g,
      JSON.stringify(pathToFileURL(path.resolve("src/model.js")).href),
    );
  await fs.writeFile(target, source);
  try {
    await import(pathToFileURL(target).href + "?t=" + Date.now());
    await new Promise((r) => setImmediate(r));
    // Nothing is on the wall until a VM is ticked in the list.
    assert.equal(win.document.querySelectorAll(".card").length, 0);
    assert.equal(
      win.document.querySelector(".vm-option-name").textContent,
      "<Lab & two monitors>",
    );
    const checkbox = win.document.querySelector("[data-select]");
    checkbox.checked = true;
    checkbox.dispatchEvent(new win.Event("change", { bubbles: true }));
    assert.match(win.localStorage.getItem("vmwall-selected"), new RegExp(id));
    assert.equal(win.document.querySelectorAll(".card").length, 1);
    assert.equal(
      win.document.querySelector(".vm-name").textContent,
      "<Lab & two monitors>",
    );
    // Ticking a VM updates the wall in place: the list row is not rebuilt.
    assert.equal(win.document.querySelector("[data-select]"), checkbox);
    const tile = win.document.querySelector(".card");
    win.document.querySelector("#search").value = "lab";
    win.document.querySelector("#search").dispatchEvent(new win.Event("input"));
    assert.equal(win.document.querySelector(".card"), tile);
    assert.equal(win.document.querySelector("#stop-live").disabled, true);
    win.document.querySelector("[data-audit]").click();
    await new Promise((r) => setImmediate(r));
    assert.equal(win.document.querySelectorAll(".monitor").length, 2);
    assert.match(win.document.querySelector("#control").textContent, /Click a monitor/);
    assert.equal(win.document.querySelector("#control").disabled, false);
    win.document.querySelector("#live-primary").click();
    await new Promise((r) => setImmediate(r));
    assert.equal(instances.at(-1).viewOnly, true);
    assert.match(instances.at(-1).url, /control=0/);
    assert.equal(win.document.querySelectorAll(".monitor").length, 2);
    const screen=win.document.querySelectorAll('.monitor .screen')[1];
    screen.dispatchEvent(new win.PointerEvent('pointerdown',{button:0,bubbles:true}));
    assert.match(win.document.querySelector('#control').textContent,/Release Display 2/);
    screen.dispatchEvent(new win.KeyboardEvent('keydown',{code:'Escape',bubbles:true}));
    assert.match(win.document.querySelector('#control').textContent,/Release Display 2/);
    assert.match(screen.querySelector('.control-hint').textContent,/Left Shift \+ Windows/);
    screen.dispatchEvent(new win.KeyboardEvent('keydown',{code:'ShiftRight',bubbles:true}));
    screen.dispatchEvent(new win.KeyboardEvent('keydown',{code:'MetaLeft',bubbles:true}));
    assert.match(win.document.querySelector('#control').textContent,/Release Display 2/);
    screen.dispatchEvent(new win.KeyboardEvent('keydown',{code:'ShiftLeft',bubbles:true}));
    assert.match(win.document.querySelector('#control').textContent,/Click a monitor/);
    assert.equal(screen.querySelector('.control-hint'),null);
    await new Promise(resolve=>setTimeout(resolve,80));

  } finally {
    await new Promise((r) => setImmediate(r));
    await win.happyDOM.abort();
    for (const [name, value] of saved) {
      if (value === undefined) delete globalThis[name];
      else globalThis[name] = value;
    }
    await win.happyDOM.abort();
    await fs.unlink(target);
  }
});
