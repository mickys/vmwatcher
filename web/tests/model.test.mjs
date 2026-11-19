import test from "node:test";
import assert from "node:assert/strict";
import { byState, canLive, liveNote, monitorsOf, parseKeys, rowsFor, wallGrid } from "../src/lib/model.js";
import { qcode } from "../src/lib/monitor-control.js";
import { decodePcm } from "../src/lib/audio-player.js";

test("the wall picks the grid that shows the most picture", () => {
  assert.deepEqual(wallGrid([16 / 9], 1600, 900), { columns: 1, rows: 1 });
  assert.deepEqual(wallGrid(Array(4).fill(16 / 9), 1600, 900), { columns: 2, rows: 2 });
  // Two wide dual-monitor tiles stack instead of sitting side by side.
  assert.deepEqual(wallGrid([3.2, 3.2], 1600, 900), { columns: 1, rows: 2 });
  assert.equal(rowsFor([2, 1, 1, 2], 2), 3);
});

test("saved selections from the single-host wall move to the local host", () => {
  const id = "0b5f1c4e-1111-4222-8333-444455556666";
  assert.deepEqual(parseKeys(JSON.stringify([id, "hv2/" + id, "../x", 5])), ["local/" + id, "hv2/" + id]);
  assert.deepEqual(parseKeys("not json"), []);
  assert.deepEqual(parseKeys(null), []);
});

test("monitor count follows the shape of the combined preview", () => {
  assert.equal(monitorsOf(1280, 800), 1);
  assert.equal(monitorsOf(2312, 800), 2);
  assert.equal(monitorsOf(100, 0), 1);
});

test("live view explains why it is unavailable", () => {
  assert.equal(liveNote({ state: "shut off", live: true }), "The VM is not running");
  assert.equal(liveNote({ state: "running", live: false, liveNote: "needs the agent" }), "needs the agent");
  assert.equal(liveNote({ state: "running", live: true }), "");
  assert.equal(canLive({ state: "paused", live: true }), true);
  const sorted = [{ state: "shut off", name: "a" }, { state: "running", name: "z" }, { state: "running", name: "b" }].sort(byState);
  assert.deepEqual(sorted.map((vm) => vm.name), ["b", "z", "a"]);
});

test("keyboard codes map to QEMU key names", () => {
  assert.equal(qcode("KeyA"), "a");
  assert.equal(qcode("Digit7"), "7");
  assert.equal(qcode("MetaLeft"), "meta_l");
  assert.equal(qcode("NumpadEnter"), "kp_enter");
  assert.equal(qcode("Unknown"), undefined);
});

test("PCM packets decode to per-channel samples", () => {
  const data = new Int16Array([16384, -16384, 0, 32767]).buffer;
  const [left, right] = decodePcm(data, 2);
  assert.deepEqual([...left], [0.5, 0]);
  assert.ok(Math.abs(right[0] + 0.5) < 1e-6 && right[1] > 0.99);
  assert.throws(() => decodePcm(new ArrayBuffer(3), 2));
});
