import test from "node:test";
import assert from "node:assert/strict";
import { byState, canLive, liveNote, monitorsOf, parseKeys, rowsFor, wallGrid } from "../src/lib/model.js";
import { qcode } from "../src/lib/monitor-control.js";
import { decodePcm } from "../src/lib/audio-player.js";
import { HOLD_BLANK_MS, acceptFrame, consoleState, frameState, isBlank, staleAfter } from "../src/lib/frame-state.js";

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
  const physical = { kind: "physical", state: "running", heads: 1, live: false };
  assert.equal(liveNote(physical, "vnc"), "");
  assert.equal(canLive(physical), false);
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

test("brief black captures keep the last good picture", () => {
  assert.equal(isBlank(new Uint8ClampedArray([0, 0, 0, 255, 7, 7, 7, 255])), true);
  assert.equal(isBlank(new Uint8ClampedArray([0, 0, 0, 255, 40, 0, 0, 255])), false);
  const memory = {};
  assert.equal(acceptFrame(memory, true, 0), true); // nothing shown yet: show even a blank
  assert.equal(acceptFrame(memory, false, 1000), true);
  assert.equal(acceptFrame(memory, true, 2000), false); // one black frame between pictures is skipped
  assert.equal(acceptFrame(memory, false, 3000), true);
  // A display that stays black is shown once it has been black for two captures and the hold time.
  assert.equal(acceptFrame(memory, true, 4000), false);
  assert.equal(acceptFrame(memory, true, 5000), false);
  assert.equal(acceptFrame(memory, true, 4000 + HOLD_BLANK_MS), true);
  assert.equal(acceptFrame(memory, true, 5000 + HOLD_BLANK_MS), true);
  // Slow previews: two captures 30 s apart both black is real.
  const slow = { shown: "picture" };
  assert.equal(acceptFrame(slow, true, 0), false);
  assert.equal(acceptFrame(slow, true, 30000), true);
});

test("frame state is live, stale or offline", () => {
  const base = { failed: false, held: false, lastFresh: 1000, delay: 30000 };
  assert.equal(frameState({ ...base, now: 2000 }), "live");
  assert.equal(frameState({ ...base, now: 1000 + staleAfter(30000) + 1 }), "stale");
  assert.equal(frameState({ ...base, held: true, now: 2000 }), "stale");
  assert.equal(frameState({ ...base, failed: true, now: 2000 }), "offline");
  assert.equal(frameState({ ...base, lastFresh: 0, now: 2000 }), "stale"); // first frame still loading
  assert.equal(frameState({ ...base, delay: 0, now: 12000 }), "stale"); // previews paused
  assert.equal(consoleState("LIVE · VIEW ONLY"), "live");
  assert.equal(consoleState("Connecting…"), "stale");
  assert.equal(consoleState("Disconnected · reconnecting"), "stale");
  assert.equal(consoleState("Playback unavailable"), "offline");
  assert.equal(consoleState("Disconnected · toggle live to retry"), "offline");
});
