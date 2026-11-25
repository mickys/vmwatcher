import { useEffect, useRef, useState } from "react";
import { useShell } from "../App.jsx";
import RFB from "@novnc/novnc";
import { vmUrl, wsUrl } from "./api.js";

// View-only noVNC console of a VM's primary monitor. Reconnects with backoff;
// a console that never connects gives up after three attempts via onGiveUp.
export function LiveConsole(props) {
  const preferred = useShell().session.settings.videoEncoder || "vnc";
  const encoder = props.vm.kind === "physical" && preferred === "vnc" ? "software-h264" : preferred;
  if (encoder === "vnc") return <VncConsole {...props} />;
  return <EncodedConsole key={encoder} encoder={encoder} {...props} />;
}

function fpsSampler(readFrames, setFps) {
  let previousFrames = readFrames();
  let lastSample = performance.now();
  return () => {
    const frames = readFrames();
    const now = performance.now();
    if (now > lastSample && frames >= previousFrames)
      setFps(Math.round(1000 * (frames - previousFrames) / (now - lastSample)));
    previousFrames = frames;
    lastSample = now;
  };
}

function FpsOverlay({ fps }) {
  return fps === null ? null : <span className="live-fps">{fps} FPS</span>;
}

function EncodedConsole({ vm, encoder, scale = true, onStatus, onGiveUp }) {
  const ref = useRef(null);
  const [fps, setFps] = useState(null);
  const callbacks = useRef({});
  callbacks.current = { onStatus, onGiveUp };
  useEffect(() => {
    const video = ref.current;
    let retry, watchdog, failures = 0, previousTime = 0, lastProgress = 0;
    const status = (message) => callbacks.current.onStatus?.(message);
    function stop() {
      clearTimeout(retry);
      clearInterval(watchdog);
      retry = undefined;
      video.removeAttribute("src");
      video.load();
    }
    function start() {
      retry = undefined;
      setFps(null);
      previousTime = -1;
      lastProgress = performance.now();
      status("Connecting…");
      video.src = vmUrl(vm, "video.mp4");
      video.load();
      const sample = typeof video.getVideoPlaybackQuality === "function" && fpsSampler(() => {
        const { totalVideoFrames, droppedVideoFrames } = video.getVideoPlaybackQuality();
        return totalVideoFrames - droppedVideoFrames;
      }, setFps);
      watchdog = setInterval(() => {
        if (video.readyState >= 2) sample && sample();
        if (video.currentTime > previousTime) {
          if (previousTime >= 0) failures = 0;
          previousTime = video.currentTime;
          lastProgress = performance.now();
        } else if (performance.now() - lastProgress >= 10000) {
          reconnect();
        }
      }, 1000);
    }
    function reconnect() {
      if (document.hidden || retry !== undefined || !video.hasAttribute("src")) return;
      stop();
      setFps(null);
      if (++failures >= 3) {
        status("Playback unavailable");
        callbacks.current.onGiveUp?.();
        return;
      }
      status("Reconnecting…");
      retry = setTimeout(start, 1000 * failures);
    }
    function visibility() {
      stop();
      if (!document.hidden) {
        failures = 0;
        start();
      }
    }
    video.addEventListener("ended", reconnect);
    video.addEventListener("error", reconnect);
    document.addEventListener("visibilitychange", visibility);
    if (!document.hidden) start();
    return () => {
      video.removeEventListener("ended", reconnect);
      video.removeEventListener("error", reconnect);
      document.removeEventListener("visibilitychange", visibility);
      stop();
    };
  }, [vm.host, vm.id]);
  return <><div className="vnc-host"><video ref={ref} muted autoPlay playsInline
    style={scale ? {width: "100%", height: "100%", objectFit: "contain"} : {}}
    onPlaying={() => onStatus?.("LIVE · " + (encoder.endsWith("265") ? "H.265" : "H.264"))} /></div><FpsOverlay fps={fps} /></>;
}

function VncConsole({ vm, scale = true, onStatus, onGiveUp }) {
  const ref = useRef(null);
  const [fps, setFps] = useState(null);
  const callbacks = useRef({});
  callbacks.current = { onStatus, onGiveUp };
  useEffect(() => {
    const node = ref.current;
    let client = null,
      timer,
      fpsTimer,
      attempts = 0,
      everConnected = false,
      authFailed = false,
      stopped = false;
    const status = (text) => callbacks.current.onStatus?.(text);
    function drop() {
      clearTimeout(timer);
      clearInterval(fpsTimer);
      const old = client;
      client = null;
      old?.disconnect();
    }
    function connect() {
      if (stopped || client || document.hidden) return;
      node.replaceChildren();
      setFps(null);
      status("Connecting…");
      const rfb = new RFB(node, wsUrl(vmUrl(vm, "vnc")), { shared: true });
      client = rfb;
      rfb.viewOnly = true;
      rfb.scaleViewport = scale;
      rfb.resizeSession = false;
      rfb.clipViewport = false;
      rfb.background = "#131923";
      rfb.addEventListener("connect", () => {
        // noVNC calls flip once after each complete framebuffer update.
        const display = rfb._display;
        if (typeof display?.flip === "function") {
          let updates = 0;
          const flip = display.flip.bind(display);
          display.flip = (...args) => {
            flip(...args);
            updates++;
          };
          fpsTimer = setInterval(fpsSampler(() => updates, setFps), 1000);
        }
        attempts = 0;
        everConnected = true;
        status("LIVE · VIEW ONLY");
      });
      rfb.addEventListener("disconnect", () => {
        if (client !== rfb) return;
        client = null;
        clearInterval(fpsTimer);
        setFps(null);
        if (stopped) return;
        if (!everConnected && attempts >= 2) return callbacks.current.onGiveUp?.();
        if (authFailed) return status("Disconnected · toggle live to retry");
        status("Disconnected · reconnecting");
        attempts++;
        timer = setTimeout(connect, Math.min(30000, 1000 * 2 ** Math.min(attempts, 5)));
      });
      rfb.addEventListener("securityfailure", () => {
        authFailed = true;
      });
      rfb.addEventListener("credentialsrequired", () => {
        const password = prompt("VNC password for " + vm.name);
        if (password !== null) rfb.sendCredentials({ password });
        else {
          authFailed = true;
          rfb.disconnect();
        }
      });
    }
    const visibility = () => (document.hidden ? drop() : connect());
    document.addEventListener("visibilitychange", visibility);
    connect();
    return () => {
      stopped = true;
      document.removeEventListener("visibilitychange", visibility);
      drop();
    };
  }, [vm.host, vm.id, scale]);
  return <><div className="vnc-host" ref={ref} /><FpsOverlay fps={fps} /></>;
}
