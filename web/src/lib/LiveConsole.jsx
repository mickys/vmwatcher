import { useEffect, useRef } from "react";
import { useShell } from "../App.jsx";
import RFB from "@novnc/novnc";
import { vmUrl, wsUrl } from "./api.js";

// View-only noVNC console of a VM's primary monitor. Reconnects with backoff;
// a console that never connects gives up after three attempts via onGiveUp.
export function LiveConsole(props) {
  const encoder = useShell().session.settings.videoEncoder;
  if (!encoder || encoder === "vnc") return <VncConsole {...props} />;
  return <EncodedConsole key={encoder} encoder={encoder} {...props} />;
}

function EncodedConsole({ vm, encoder, scale = true, onStatus }) {
  const ref = useRef(null);
  useEffect(() => {
    const video = ref.current;
    function update() {
      if (document.hidden) video.removeAttribute("src");
      else video.src = vmUrl(vm, "video.mp4");
      video.load();
    }
    update();
    document.addEventListener("visibilitychange", update);
    return () => {
      document.removeEventListener("visibilitychange", update);
      video.removeAttribute("src");
      video.load();
    };
  }, [vm.host, vm.id]);
  return <div className="vnc-host"><video ref={ref} muted autoPlay playsInline
    style={scale ? {width: "100%", height: "100%", objectFit: "contain"} : {}}
    onPlaying={() => onStatus?.("LIVE · " + (encoder.endsWith("265") ? "H.265" : "H.264"))}
    onError={() => onStatus?.("Playback unavailable · check encoder and browser codec support")} /></div>;
}

function VncConsole({ vm, scale = true, onStatus, onGiveUp }) {
  const ref = useRef(null);
  const callbacks = useRef({});
  callbacks.current = { onStatus, onGiveUp };
  useEffect(() => {
    const node = ref.current;
    let client = null,
      timer,
      attempts = 0,
      everConnected = false,
      authFailed = false,
      stopped = false;
    const status = (text) => callbacks.current.onStatus?.(text);
    function drop() {
      clearTimeout(timer);
      const old = client;
      client = null;
      old?.disconnect();
    }
    function connect() {
      if (stopped || client || document.hidden) return;
      node.replaceChildren();
      status("Connecting…");
      const rfb = new RFB(node, wsUrl(vmUrl(vm, "vnc")), { shared: true });
      client = rfb;
      rfb.viewOnly = true;
      rfb.scaleViewport = scale;
      rfb.resizeSession = false;
      rfb.clipViewport = false;
      rfb.background = "#131923";
      rfb.addEventListener("connect", () => {
        attempts = 0;
        everConnected = true;
        status("LIVE · VIEW ONLY");
      });
      rfb.addEventListener("disconnect", () => {
        if (client !== rfb) return;
        client = null;
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
  return <div className="vnc-host" ref={ref} />;
}
