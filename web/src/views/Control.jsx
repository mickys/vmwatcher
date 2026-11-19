import { Bandwidth } from "../lib/Bandwidth.jsx";
import { useCallback, useEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";
import { useShell } from "../App.jsx";
import { api, vmUrl, viewerUrl } from "../lib/api.js";
import { Frame } from "../lib/Frame.jsx";
import { LiveConsole } from "../lib/LiveConsole.jsx";
import { keepAudio, toggleAudio, useAudio } from "../lib/audio-store.js";
import { bindMonitorControl } from "../lib/monitor-control.js";
import { byState, canLive, isActive, liveNote } from "../lib/model.js";

// Every monitor of one VM at full size: per-monitor captures, an optional live
// console for the primary monitor, keyboard/mouse control and saved audits.
export function Control({ vmKey }) {
  const { inventory } = useShell();
  const vm = inventory.vms.find((item) => item.key === vmKey);
  useEffect(() => {
    // Without a VM in the address, open the first running one.
    if (!vmKey && inventory.vms.length) {
      const first = [...inventory.vms].sort(byState)[0];
      location.replace("#/control/" + first.key);
    }
  }, [vmKey, inventory.vms]);
  if (!vm)
    return (
      <main className="page">
        <div className="empty">
          <strong>{inventory.loaded ? (vmKey ? "This VM is not available" : "No VMs to control") : "Loading…"}</strong>
          {inventory.loaded && vmKey && "It may have been removed, or its host is unreachable."}
        </div>
      </main>
    );
  return <ControlVm key={vm.key} vm={vm} />;
}

function ControlVm({ vm }) {
  const { session, inventory, notify, bar } = useShell();
  const controlEnabled = session.settings.controlEnabled;
  const [native, setNative] = useState(false);
  const [live, setLive] = useState(false);
  const [busy, setBusy] = useState(false);
  const [records, setRecords] = useState([]);
  const [saved, setSaved] = useState(null);
  const [frames, setFrames] = useState({});
  const [liveStatus, setLiveStatus] = useState("");
  const monitorsRef = useRef(null);
  const controlButton = useRef(null);
  const dialog = useRef(null);
  const audio = useAudio().get(vm.key);
  const note = liveNote(vm, session.settings.videoEncoder);
  const liveOn = live && !note;

  const history = useCallback(async () => {
    try {
      setRecords((await api("/api/audits?vm=" + encodeURIComponent(vm.key))).audits);
    } catch (error) {
      notify(error.message, true);
    }
  }, [vm.key, notify]);
  useEffect(() => {
    history();
    keepAudio(new Set([vm.key]));
    return () => keepAudio(new Set());
  }, [history, vm.key]);

  // The control library drives the monitors and its button directly, so React
  // leaves that button's contents alone.
  useEffect(() => {
    if (!controlEnabled) return;
    return bindMonitorControl(
      monitorsRef.current,
      controlButton.current,
      (head, events) => api(vmUrl(vm, "input"), { method: "POST", body: { head, events } }),
      (message) => notify(message, true),
    );
  }, [vm.host, vm.id, vm.heads, controlEnabled, notify]);

  useEffect(() => {
    if (saved) dialog.current?.showModal();
  }, [saved]);

  async function capture() {
    setBusy(true);
    try {
      const record = await api(vmUrl(vm, "capture"), { method: "POST" });
      notify(
        `Saved ${record.displays.length} display${record.displays.length === 1 ? "" : "s"}` +
          (record.unavailable.length ? ` · ${record.unavailable.length} unavailable` : "") +
          ".",
      );
      await history();
      setSaved(record);
    } catch (error) {
      notify(error.message, true);
    }
    setBusy(false);
  }

  const controls = (
    <>
      <select
        id="audit-vm"
        aria-label="VM to control"
        value={vm.key}
        onChange={(e) => (location.hash = "#/control/" + e.target.value)}
      >
        {inventory.hosts.map((host) => (
          <optgroup key={host.id} label={host.name}>
            {inventory.vms
              .filter((item) => item.host === host.id)
              .sort(byState)
              .map((item) => (
                <option key={item.key} value={item.key}>
                  {item.name} · {item.state}
                </option>
              ))}
          </optgroup>
        ))}
      </select>
      <button onClick={() => setNative(!native)}>{native ? "Fit monitors" : "Native pixels"}</button>
      <button disabled={!!note} title={note} onClick={() => setLive(!liveOn)}>
        {liveOn ? "Stop live primary" : "Live primary"}
      </button>
      {controlEnabled ? (
        <button ref={controlButton} />
      ) : (
        <button disabled title="Keyboard and mouse control is turned off in Settings">
          Control is turned off
        </button>
      )}
      <button
        className={audio ? "chosen" : ""}
        aria-pressed={!!audio}
        disabled={!canLive(vm)}
        title={audio ? audio + " · click to mute" : "Listen to this VM"}
        onClick={() => toggleAudio(vm, (message) => notify(message, true))}
      >
        {audio ? "Mute audio" : "Audio"}
      </button>
      <button id="capture" className="primary" disabled={busy || !isActive(vm)} onClick={capture}>
        Save full audit
      </button>
    </>
  );

  return (
    <>
      {bar && createPortal(controls, bar)}
      <main className="audit-layout">
        <section
          ref={monitorsRef}
          className={"monitors" + (native ? " native" : "")}
          style={{ "--heads": Math.min(vm.heads, 2) }}
        >
          {Array.from({ length: vm.heads }, (_, head) => {
            const onConsole = head === 0 && liveOn;
            const frame = frames[head];
            return (
              <article className="monitor" key={head}>
                <header>
                  <strong>Display {head + 1}</strong>
                  <Bandwidth scope={vm.key + "/" + head} />
                  <span className="resolution">{frame && !onConsole ? `${frame.width} × ${frame.height}` : `Head ${head}`}</span>
                  <span className="live-status">{onConsole ? liveStatus || "Connecting…" : "per-head capture"}</span>
                  <span className="frame-time">
                    {onConsole ? "Live console · primary head" : frame ? "Received " + frame.time : "Waiting for frame…"}
                  </span>
                </header>
                <div className="screen">
                  {onConsole ? (
                    <LiveConsole
                      vm={vm}
                      scale={!native}
                      onStatus={setLiveStatus}
                      onGiveUp={() => {
                        setLive(false);
                        notify(
                          vm.name + ": the console refused the live viewer. Another VNC client may be holding it exclusively.",
                          true,
                        );
                      }}
                    />
                  ) : isActive(vm) ? (
                    <Frame
                      src={vmUrl(vm, `heads/${head}.png`)}
                      delay={1000}
                      alt={`${vm.name} display ${head + 1}`}
                      onFrame={(width, height) =>
                        setFrames((old) => ({ ...old, [head]: { width, height, time: new Date().toLocaleTimeString() } }))
                      }
                    />
                  ) : (
                    <div className="off-screen">{vm.state === "shut off" ? "Powered off" : vm.state}</div>
                  )}
                </div>
              </article>
            );
          })}
        </section>
        <aside className="audit-side">
          <h2>Saved audits</h2>
          <p>PNG for each head, native combined image, timestamps and SHA-256 manifest.</p>
          <div id="history">
            {records.map((record) => (
              <button className="history-item" key={record.id} onClick={() => setSaved(record)}>
                <img src={viewerUrl(`/api/audits/${record.id}/preview.png`)} loading="lazy" alt="Saved audit preview" />
                <span>
                  {new Date(record.startedAt).toLocaleString()}
                  <small>{record.displays.map((d) => d.width + "×" + d.height).join(" + ")}</small>
                </span>
              </button>
            ))}
            {!records.length && <p className="muted">No saved audits yet.</p>}
          </div>
          <div className="audit-help">
            <strong>{controlEnabled ? "Click a monitor to control" : "View only"}</strong>
            {controlEnabled && (
              <p>
                Click either display to send mouse and keyboard input. Press Left Shift + Windows key or click outside
                the display to release control.
              </p>
            )}
            <p>
              Saving an audit captures heads one after another and records each timestamp. Configured heads may include
              an inactive or blank display; nothing is inferred from a black image.
            </p>
          </div>
        </aside>
      </main>
      {saved && (
        <dialog ref={dialog} id="saved-dialog" onClose={() => setSaved(null)}>
          <div className="dialog-top">
            <strong>Saved display audit</strong>
            <button onClick={() => dialog.current.close()}>Close</button>
          </div>
          <img id="saved-image" src={viewerUrl(`/api/audits/${saved.id}/combined.png`)} alt="Saved native-resolution display audit" />
          <p>
            {saved.vm.name} · {new Date(saved.startedAt).toLocaleString()} ·{" "}
            {saved.displays.map((d) => `Display ${d.head + 1} ${d.width}×${d.height}`).join(" / ")}
          </p>
          <a href={`/api/audits/${saved.id}/bundle.zip`} download={`vm-audit-${saved.id}.zip`}>
            Download PNGs + manifest
          </a>
        </dialog>
      )}
    </>
  );
}
