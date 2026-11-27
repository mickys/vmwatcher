import { useEffect, useRef, useState } from "react";
import { FRAME_STATES, acceptFrame, frameState, isBlank } from "./frame-state.js";

// Samples a decoded image at low resolution; false when it cannot be read.
function blankImage(image) {
  try {
    const canvas = document.createElement("canvas");
    canvas.width = 64;
    canvas.height = 36;
    const context = canvas.getContext("2d", { willReadFrequently: true });
    if (!context) return false;
    context.drawImage(image, 0, 0, canvas.width, canvas.height);
    return isBlank(context.getImageData(0, 0, canvas.width, canvas.height).data);
  } catch {
    return false;
  }
}

// A periodically refreshed screenshot. The next frame is fetched and decoded
// off-screen and only then swapped in, so the image never blanks between frames.
// A briefly all-black capture keeps the last good picture (see acceptFrame).
// `onState` receives "live", "stale" or "offline" whenever that changes.
export function Frame({ src, delay, alt, sized = false, onFrame, onState }) {
  const ref = useRef(null);
  const report = useRef(onFrame);
  report.current = onFrame;
  const reportState = useRef(onState);
  reportState.current = onState;
  const [failed, setFailed] = useState(false);
  useEffect(() => {
    const img = ref.current;
    const memory = {};
    const status = { failed: false, held: false, lastFresh: 0 };
    let stopped = false,
      timer,
      shown = null,
      published = null;
    function publish() {
      const next = frameState({ ...status, delay, now: Date.now() });
      if (next === published) return;
      published = next;
      reportState.current?.(next);
    }
    const clock = setInterval(publish, 1000);
    publish();
    async function tick() {
      if (stopped) return;
      const rect = img.getBoundingClientRect();
      // Hidden tabs and off-screen tiles do not poll.
      if (document.hidden || rect.width === 0 || rect.bottom < 0 || rect.top > innerHeight) {
        timer = setTimeout(tick, 500);
        return;
      }
      const url = new URL(src, location.href);
      url.searchParams.set("t", Date.now());
      // Widths are rounded up to steps so viewers of similar size share server work.
      if (sized)
        url.searchParams.set(
          "width",
          Math.min(3840, Math.max(320, Math.ceil((rect.width * (window.devicePixelRatio || 1)) / 160) * 160)),
        );
      try {
        const response = await fetch(url);
        if (!response.ok) throw Error("Display unavailable");
        const frame = URL.createObjectURL(await response.blob());
        const probe = new Image();
        probe.src = frame;
        await probe.decode();
        if (stopped) return URL.revokeObjectURL(frame);
        status.failed = false;
        setFailed(false);
        if (acceptFrame(memory, blankImage(probe), Date.now())) {
          const previous = shown;
          shown = frame;
          img.onload = () => previous && URL.revokeObjectURL(previous);
          img.src = frame;
          status.held = false;
          status.lastFresh = Date.now();
          report.current?.(probe.naturalWidth, probe.naturalHeight);
        } else {
          URL.revokeObjectURL(frame);
          status.held = true;
        }
      } catch {
        if (stopped) return;
        status.failed = true;
        setFailed(true);
      }
      publish();
      if (!stopped && delay) timer = setTimeout(tick, delay);
    }
    tick();
    return () => {
      stopped = true;
      clearTimeout(timer);
      clearInterval(clock);
      if (shown) URL.revokeObjectURL(shown);
    };
  }, [src, delay, sized]);
  return (
    <>
      <img ref={ref} alt={alt} />
      {failed && <span className="image-error">Display unavailable · retrying</span>}
    </>
  );
}

// Green / orange / red marker for a display's picture.
export function FrameIndicator({ state, detail }) {
  const info = FRAME_STATES[state] || FRAME_STATES.stale;
  return (
    <span className={"frame-state " + (FRAME_STATES[state] ? state : "stale")} role="status"
      title={info.title + (detail ? " · " + detail : "")}>
      {info.label}
    </span>
  );
}
