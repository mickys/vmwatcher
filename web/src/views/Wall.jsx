import { Bandwidth } from "../lib/Bandwidth.jsx";
import { memo, useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from "react";
import { createPortal } from "react-dom";
import { useShell } from "../App.jsx";
import { api, vmUrl } from "../lib/api.js";
import { Frame } from "../lib/Frame.jsx";
import { LiveConsole } from "../lib/LiveConsole.jsx";
import { keepAudio, toggleAudio, useAudio } from "../lib/audio-store.js";
import { byState, canLive, isActive, liveNote, monitorsOf, parseKeys, wallGrid } from "../lib/model.js";

const stored = (name, legacy) => new Set(parseKeys(localStorage.getItem(name) ?? localStorage.getItem(legacy)));

export function Wall() {
  const { session, inventory, groups, setGroups, notify, bar } = useShell();
  const encoder = session.settings.videoEncoder;
  const supportsLive = (vm) => !liveNote(vm, encoder);
  const maxLive = session.settings.maxLivePerSession;
  const [selected, setSelected] = useState(() => stored("vmw-selected", "vmwall-selected"));
  // What was ticked before a group was shown; restored when it is toggled off.
  const [manual, setManual] = useState(() => stored("vmw-manual", "vmwall-manual"));
  const [group, setGroup] = useState(() => localStorage.getItem("vmw-group") ?? localStorage.getItem("vmwall-group") ?? "");
  const [live, setLive] = useState(() => new Set());
  const [rate, setRate] = useState(session.settings.previewSeconds * 1000);
  const [query, setQuery] = useState("");
  const [sidebar, setSidebar] = useState(() => localStorage.getItem("vmw-sidebar") !== "0");
  const [shapes, setShapes] = useState(() => new Map());
  const [size, setSize] = useState({ width: 0, height: 0 });
  const [capturing, setCapturing] = useState(false);
  const wallRef = useRef(null);
  const audio = useAudio();

  useEffect(() => {
    localStorage.setItem("vmw-selected", JSON.stringify([...selected]));
    localStorage.setItem("vmw-manual", JSON.stringify([...manual]));
    localStorage.setItem("vmw-group", group);
  }, [selected, manual, group]);
  useEffect(() => localStorage.setItem("vmw-sidebar", sidebar ? "1" : "0"), [sidebar]);

  const shown = useMemo(
    () => inventory.vms.filter((vm) => selected.has(vm.key)).sort(byState),
    [inventory.vms, selected],
  );
  // Live consoles and audio only make sense for VMs that are on the wall.
  useEffect(() => {
    const keys = new Set(shown.map((vm) => vm.key));
    keepAudio(keys);
    setLive((old) => {
      const next = new Set([...old].filter((key) => keys.has(key)));
      return next.size === old.size ? old : next;
    });
  }, [shown]);
  useEffect(() => () => keepAudio(new Set()), []);

  useLayoutEffect(() => {
    const element = wallRef.current;
    const observer = new ResizeObserver(() => setSize({ width: element.clientWidth, height: element.clientHeight }));
    observer.observe(element);
    return () => observer.disconnect();
  }, []);

  const aspectOf = (vm) => shapes.get(vm.key)?.aspect || (16 / 9) * (isActive(vm) ? Math.min(vm.heads, 4) : 1);
  const grid = wallGrid(shown.map(aspectOf), size.width, size.height);

  const onShape = useCallback((key, width, height) => {
    if (!height) return;
    setShapes((old) => {
      const aspect = width / height,
        monitors = monitorsOf(width, height),
        known = old.get(key);
      if (known && Math.abs(known.aspect - aspect) < 0.001 && known.monitors === monitors) return old;
      return new Map(old).set(key, { aspect, monitors });
    });
  }, []);
  const toggleLive = useCallback(
    (key) =>
      setLive((old) => {
        const next = new Set(old);
        if (next.delete(key)) return next;
        if (old.size >= maxLive) {
          notify(`Choose up to ${maxLive} live consoles.`, true);
          return old;
        }
        return next.add(key);
      }),
    [maxLive, notify],
  );
  const giveUp = useCallback(
    (vm) => {
      setLive((old) => {
        const next = new Set(old);
        next.delete(vm.key);
        return next;
      });
      notify(
        vm.name +
          ": the console refused the live viewer, so the tile is back on previews. Another VNC client may be holding it exclusively.",
        true,
      );
    },
    [notify],
  );
  const onAudio = useCallback((vm) => toggleAudio(vm, (message) => notify(message, true)), [notify]);

  function tick(key, on) {
    setSelected((old) => {
      const next = new Set(old);
      on ? next.add(key) : next.delete(key);
      return next;
    });
  }
  const currentGroup = groups.find((g) => g.name === group);
  // Clicking a group shows exactly its VMs; clicking it again restores the previous selection.
  function toggleGroup(name) {
    const target = groups.find((g) => g.name === name);
    if (!target) return;
    if (group === name) {
      setGroup("");
      setSelected(new Set(manual));
    } else {
      if (!currentGroup) setManual(new Set(selected));
      setGroup(name);
      setSelected(new Set(target.vms));
    }
  }
  async function saveGroup(name) {
    try {
      const data = await api("/api/groups/" + encodeURIComponent(name), { method: "PUT", body: { vms: [...selected] } });
      if (!currentGroup) setManual(new Set(selected));
      setGroups(data.groups);
      setGroup(name);
      notify(`Group “${name}” saved with ${selected.size} VM${selected.size === 1 ? "" : "s"}.`);
    } catch (error) {
      notify(error.message, true);
    }
  }
  function askSaveGroup() {
    const name = (prompt("Save the selected VMs to group:", group) || "").trim();
    if (!name) return;
    if (
      name !== group &&
      groups.some((g) => g.name === name) &&
      !confirm(`Group “${name}” already exists. Replace its VMs with the selected ones?`)
    )
      return;
    saveGroup(name);
  }
  async function deleteGroup() {
    if (!currentGroup || !confirm(`Delete the group “${group}”? The VMs themselves are not affected.`)) return;
    try {
      const data = await api("/api/groups/" + encodeURIComponent(group), { method: "DELETE" });
      setGroups(data.groups);
      setGroup("");
      notify(`Group “${group}” deleted.`);
    } catch (error) {
      notify(error.message, true);
    }
  }
  async function captureSelected() {
    const targets = shown.filter(isActive);
    if (targets.length > 20) return notify("Capture up to twenty selected VMs in one batch.", true);
    setCapturing(true);
    let done = 0;
    for (const vm of targets) {
      try {
        await api(vmUrl(vm, "capture"), { method: "POST" });
        done++;
      } catch (error) {
        notify(vm.name + ": " + error.message, true);
      }
    }
    setCapturing(false);
    notify(`Saved ${done}/${targets.length} native-resolution audits. Open a VM in Take Control to download them.`);
  }

  const sameAsGroup =
    currentGroup && currentGroup.vms.length === selected.size && currentGroup.vms.every((key) => selected.has(key));
  const controls = (
    <>
      <button
        id="toggle-sidebar"
        aria-pressed={sidebar}
        title={(sidebar ? "Hide" : "Show") + " the VM list"}
        onClick={() => setSidebar(!sidebar)}
      >
        ☰
      </button>
      <span id="selection-count">{shown.length} selected</span>
      <button disabled={!selected.size} onClick={askSaveGroup} title="Save the selected VMs as a named group">
        Save to group
      </button>
      {currentGroup && (
        <>
          <button disabled={sameAsGroup} onClick={() => saveGroup(group)} title="Replace this group's VMs with the selected ones">
            Update “{group}”
          </button>
          <button onClick={deleteGroup} title="Delete this group">
            Delete group
          </button>
        </>
      )}
      <label>
        Preview{" "}
        <select value={rate} onChange={(e) => setRate(Number(e.target.value))}>
          {[...new Set([0, 2000, 5000, 10000, 30000, session.settings.previewSeconds * 1000])]
            .sort((a, b) => a - b)
            .map((ms) => (
              <option key={ms} value={ms}>
                {ms ? ms / 1000 + " seconds" : "Paused"}
              </option>
            ))}
        </select>
      </label>
      <button
        disabled={!shown.some(supportsLive)}
        title={`Live view-only console of the primary display for every selected VM (up to ${maxLive})`}
        onClick={() => {
          const keys = shown.filter(supportsLive).map((vm) => vm.key);
          setLive(new Set(keys.slice(0, maxLive)));
          if (keys.length > maxLive) notify(`Showing the first ${maxLive} selected live consoles.`);
        }}
      >
        Live selected
      </button>
      <button disabled={!live.size} onClick={() => setLive(new Set())}>
        Stop live
      </button>
      <span id="live-count">
        Live {live.size}/{maxLive}
      </span>
      <button
        id="capture-selected"
        className="primary"
        disabled={capturing || !shown.some(isActive)}
        title="Save a native-resolution audit of every selected running VM"
        onClick={captureSelected}
      >
        Capture selected
      </button>
    </>
  );

  return (
    <>
      {bar && createPortal(controls, bar)}
      <main className={"wall-layout" + (sidebar ? "" : " no-sidebar")}>
        <Sidebar
          query={query}
          setQuery={setQuery}
          selected={selected}
          tick={tick}
          group={group}
          toggleGroup={toggleGroup}
        />
        <section className="wall" ref={wallRef} style={{ "--wall-columns": grid.columns, "--wall-rows": grid.rows }}>
          {shown.map((vm) => (
            <Card
              encoder={encoder}
              key={vm.key}
              vm={vm}
              live={live.has(vm.key) && supportsLive(vm)}
              rate={rate}
              aspect={aspectOf(vm)}
              monitors={shapes.get(vm.key)?.monitors ?? (isActive(vm) ? Math.min(vm.heads, 4) : 1)}
              audio={audio.get(vm.key)}
              multiHost={inventory.hosts.length > 1}
              hostName={inventory.hosts.find((host) => host.id === vm.host)?.name}
              onShape={onShape}
              onLive={toggleLive}
              onGiveUp={giveUp}
              onAudio={onAudio}
            />
          ))}
          {!shown.length && (
            <div className="empty">
              <strong>{inventory.loaded ? "No VMs on the wall yet" : "Loading VMs…"}</strong>
              {inventory.loaded &&
                (inventory.hosts.length
                  ? "Tick the VMs you want to watch in the list. Your selection is remembered in this browser."
                  : "No hosts are configured yet. Add one in Settings.")}
            </div>
          )}
        </section>
      </main>
    </>
  );
}

function Sidebar({ query, setQuery, selected, tick, group, toggleGroup }) {
  const { inventory, groups } = useShell();
  const needle = query.toLowerCase();
  const matches = (vm) => vm.name.toLowerCase().includes(needle);
  const several = inventory.hosts.length > 1;
  return (
    <div className="vm-sidebar">
      <input
        type="search"
        placeholder="Find a VM…"
        aria-label="Find a VM"
        value={query}
        onChange={(e) => setQuery(e.target.value)}
      />
      <div className="vm-list">
        {groups
          .filter((g) => g.name.toLowerCase().includes(needle))
          .map((g) => (
            <button
              key={g.name}
              className={"group-option" + (g.name === group ? " checked" : "")}
              aria-pressed={g.name === group}
              title={
                g.name === group
                  ? "Showing this group · click to go back to your own selection"
                  : "Show this group on the wall"
              }
              onClick={() => toggleGroup(g.name)}
            >
              <span className="group-name">{g.name}</span>
              <span className="group-count">{g.vms.length}</span>
            </button>
          ))}
        {inventory.hosts.map((host) => {
          const vms = inventory.vms.filter((vm) => vm.host === host.id && matches(vm)).sort(byState);
          if (!host.enabled || (needle && !vms.length)) return null;
          return (
            <div key={host.id} className="host-block">
              {(several || !host.ok) && (
                <div className={"host-heading" + (host.ok ? "" : " down")} title={host.error || host.name}>
                  <span className="host-name">{host.name}</span>
                  <span className="host-count">
                    {host.ok ? `${vms.filter((vm) => vm.state === "running").length} / ${vms.length}` : "unreachable"}
                  </span>
                </div>
              )}
              {!host.ok && <div className="host-error">{host.error}</div>}
              {vms.map((vm) => (
                <label
                  key={vm.key}
                  className={"vm-option" + (selected.has(vm.key) ? " checked" : "")}
                  title={`${vm.name} · ${vm.state}`}
                >
                  <input
                    type="checkbox"
                    aria-label={`Show ${vm.name} (${vm.state})`}
                    checked={selected.has(vm.key)}
                    onChange={(e) => tick(vm.key, e.target.checked)}
                  />
                  <span className={"dot " + vm.state} />
                  <span className="vm-option-name">{vm.name}</span>
                </label>
              ))}
            </div>
          );
        })}
        {inventory.loaded && !inventory.vms.some(matches) && <div className="empty-list">No matching VMs.</div>}
      </div>
    </div>
  );
}

// One VM on the wall. Preview: a single image with every monitor side by side.
// Live: VNC only carries the primary monitor, so the others stay on fast
// per-monitor captures.
const Card = memo(function Card({ vm, encoder, live, rate, aspect, monitors, audio, multiHost, hostName, onShape, onLive, onGiveUp, onAudio }) {
  const [status, setStatus] = useState("preview");
  useEffect(() => {
    if (!live) setStatus("preview");
  }, [live]);
  const note = liveNote(vm, encoder);
  const title = (multiHost ? hostName + " · " : "") + `${vm.name} · ${vm.state} · open monitor controls`;
  const control = "#/control/" + vm.key;
  return (
    <article className="card" style={{ "--card-aspect": aspect }}>
      <header>
        <span className={"dot " + vm.state} title={vm.state} />
        <a className="vm-name" href={control} title={title}>
          {vm.name}
        </a>
        <Bandwidth scope={vm.key} />
        <span className="card-status">
          {multiHost && hostName + " · "}
          {monitors > 1 && monitors + " monitors · "}
          <span className="live-status">{status}</span>
        </span>
        <a className="button" href={control}>
          Take Control
        </a>
        <button
          className={audio ? "chosen" : ""}
          aria-pressed={!!audio}
          disabled={!canLive(vm)}
          title={audio ? audio + " · click to mute" : "Listen to this VM"}
          onClick={() => onAudio(vm)}
        >
          {audio ? "Mute audio" : "Audio"}
        </button>
        <button
          disabled={!!note}
          title={
            note ||
            "Live view-only console of the primary monitor" +
              (monitors > 1 ? "; the other monitors refresh every second" : "")
          }
          onClick={() => onLive(vm.key)}
        >
          {live ? "Stop" : "Live"}
        </button>
      </header>
      <div className={"card-screen" + (live ? " live" : "")}>
        {live ? (
          <>
            <div className="pane">
              <LiveConsole vm={vm} onStatus={setStatus} onGiveUp={() => onGiveUp(vm)} />
            </div>
            {Array.from({ length: monitors - 1 }, (_, n) => (
              <div className="pane" key={n}>
                <Frame src={vmUrl(vm, `heads/${n + 1}.png`)} delay={1000} alt={`${vm.name} display ${n + 2}`} />
              </div>
            ))}
          </>
        ) : isActive(vm) ? (
          <Frame
            src={vmUrl(vm, "preview.png")}
            delay={rate}
            sized
            alt={vm.name + " display previews"}
            onFrame={(width, height) => onShape(vm.key, width, height)}
          />
        ) : (
          <div className="off-screen">{vm.state === "shut off" ? "Powered off" : vm.state}</div>
        )}
      </div>
    </article>
  );
});
