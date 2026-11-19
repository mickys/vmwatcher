import { useSyncExternalStore } from "react";
import { AudioPlayer } from "./audio-player.js";
import { vmUrl, wsUrl } from "./api.js";

// Which VMs are playing audio, shared by the wall and the control view.
const players = new Map();
const listeners = new Set();
let snapshot = new Map();
function changed() {
  snapshot = new Map([...players].map(([key, player]) => [key, player.status]));
  for (const listener of listeners) listener();
}

export function stopAudio(key) {
  const player = players.get(key);
  if (!player) return;
  players.delete(key);
  player.stop();
  changed();
}

export function toggleAudio(vm, report) {
  if (players.has(vm.key)) return stopAudio(vm.key);
  const player = new AudioPlayer(wsUrl(vmUrl(vm, "audio")), (error) => {
    if (player.stopped && players.get(vm.key) === player) players.delete(vm.key);
    changed();
    if (error) report(vm.name + ": " + error);
  });
  players.set(vm.key, player);
  changed();
  player.start();
}

// Stops audio for every VM that is not in `keep`.
export function keepAudio(keep) {
  for (const key of [...players.keys()]) if (!keep.has(key)) stopAudio(key);
}

export function useAudio() {
  return useSyncExternalStore(
    (listener) => {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    () => snapshot,
  );
}
