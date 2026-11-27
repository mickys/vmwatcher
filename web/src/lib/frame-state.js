// How fresh a displayed picture is, and which captured frames are worth showing.

// A frame whose sampled pixels are all this dark counts as blank.
const BLANK_LEVEL = 12;
// A blank frame only replaces a picture once the display has stayed blank for
// this long and for at least two captures in a row; brief blanks are skipped.
export const HOLD_BLANK_MS = 10000;

// `pixels` is RGBA data, as from getImageData.
export function isBlank(pixels) {
  for (let i = 0; i < pixels.length; i += 4)
    if (pixels[i] > BLANK_LEVEL || pixels[i + 1] > BLANK_LEVEL || pixels[i + 2] > BLANK_LEVEL) return false;
  return pixels.length > 0;
}

// Decides whether a newly captured frame replaces the shown one. `memory`
// persists between calls for one display: { shown, blankSince, blankCount }.
// Returns true to show the frame, false to keep the last good picture.
export function acceptFrame(memory, blank, now) {
  if (!blank) {
    memory.blankSince = null;
    memory.blankCount = 0;
    memory.shown = "picture";
    return true;
  }
  memory.blankSince ??= now;
  memory.blankCount = (memory.blankCount || 0) + 1;
  const settled = memory.blankCount >= 2 && now - memory.blankSince >= HOLD_BLANK_MS;
  if (memory.shown === "picture" && !settled) return false;
  memory.shown = "blank";
  return true;
}

// A picture is stale once no new frame has arrived for this long.
export function staleAfter(delay) {
  return delay ? Math.max(2 * delay + 5000, 10000) : 10000;
}

// "live" (green), "stale" (orange) or "offline" (red) for a refreshed screenshot.
export function frameState({ failed, held, lastFresh, delay, now }) {
  if (failed || !lastFresh) return failed ? "offline" : "stale";
  if (held || now - lastFresh > staleAfter(delay)) return "stale";
  return "live";
}

// The same for a live console, from the status text it reports.
export function consoleState(status) {
  if (/^LIVE/.test(status || "")) return "live";
  if (/unavailable|toggle live/i.test(status || "")) return "offline";
  return "stale";
}

export const FRAME_STATES = {
  live: { label: "Live", title: "Picture is up to date" },
  stale: { label: "Stale", title: "Picture is not current: waiting for a new frame, or a blank frame was skipped" },
  offline: { label: "Offline", title: "No picture: the display is unavailable or the machine is not running" },
};
