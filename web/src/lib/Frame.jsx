import { useEffect, useRef, useState } from "react";

// A periodically refreshed screenshot. The next frame is fetched and decoded
// off-screen and only then swapped in, so the image never blanks between frames.
export function Frame({ src, delay, alt, sized = false, onFrame }) {
  const ref = useRef(null);
  const report = useRef(onFrame);
  report.current = onFrame;
  const [failed, setFailed] = useState(false);
  useEffect(() => {
    const img = ref.current;
    let stopped = false,
      timer,
      shown = null;
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
        const previous = shown;
        shown = frame;
        img.onload = () => previous && URL.revokeObjectURL(previous);
        img.src = frame;
        setFailed(false);
        report.current?.(probe.naturalWidth, probe.naturalHeight);
      } catch {
        if (!stopped) setFailed(true);
      }
      if (!stopped && delay) timer = setTimeout(tick, delay);
    }
    tick();
    return () => {
      stopped = true;
      clearTimeout(timer);
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
