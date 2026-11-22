import { Bandwidth, BandwidthProvider } from "./lib/Bandwidth.jsx";
import { createContext, useCallback, useContext, useEffect, useRef, useState } from "react";
import { api, onUnauthorized } from "./lib/api.js";
import { Wall } from "./views/Wall.jsx";
import { Control } from "./views/Control.jsx";
import { Settings } from "./views/Settings.jsx";

export const ShellContext = createContext(null);
export const useShell = () => useContext(ShellContext);

function parseRoute() {
  const control = location.hash.match(/^#\/control\/([a-z0-9-]{1,32})\/([0-9a-f-]{36})$/);
  if (control) return { view: "control", key: control[1] + "/" + control[2] };
  if (location.hash === "#/control") return { view: "control", key: null };
  if (location.hash === "#/settings") return { view: "settings" };
  return { view: "wall" };
}

export function App() {
  const [session, setSession] = useState(null);
  const [problem, setProblem] = useState("");
  const load = useCallback(async () => {
    try {
      setSession(await api("/api/session"));
      setProblem("");
    } catch (error) {
      setProblem(error.message);
    }
  }, []);
  useEffect(() => {
    onUnauthorized(load);
    load();
  }, [load]);
  if (!session)
    return (
      <div className="gate">
        <div className="gate-card">
          <h1>VM Watcher</h1>
          <p className={problem ? "form-error" : "muted"}>{problem || "Loading…"}</p>
          {problem && <button onClick={load}>Retry</button>}
        </div>
      </div>
    );
  if (session.setupRequired || !session.authenticated) return <SignIn session={session} done={load} />;
  return <Shell session={session} reload={load} />;
}

function SignIn({ session, done }) {
  const setup = session.setupRequired;
  const [password, setPassword] = useState("");
  const [again, setAgain] = useState("");
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  async function submit(event) {
    event.preventDefault();
    if (setup && password !== again) return setError("The two passwords do not match");
    setBusy(true);
    setError("");
    try {
      await api(setup ? "/api/setup" : "/api/login", { method: "POST", body: { password } });
      await done();
    } catch (failure) {
      setError(failure.message);
      setBusy(false);
    }
  }
  return (
    <div className="gate">
      <form className="gate-card" onSubmit={submit}>
        <h1>VM Watcher</h1>
        <p className="muted">
          {setup
            ? "Welcome. Choose the password everyone will use to open this viewer."
            : "Enter the viewer password to continue."}
        </p>
        <label>
          {setup ? "New password" : "Password"}
          <input
            type="password"
            autoFocus
            required
            minLength={setup ? 8 : 1}
            autoComplete={setup ? "new-password" : "current-password"}
            value={password}
            onChange={(e) => setPassword(e.target.value)}
          />
        </label>
        {setup && (
          <label>
            Repeat password
            <input
              type="password"
              required
              autoComplete="new-password"
              value={again}
              onChange={(e) => setAgain(e.target.value)}
            />
          </label>
        )}
        {error && (
          <p className="form-error" role="alert">
            {error}
          </p>
        )}
        <button className="primary" disabled={busy}>
          {setup ? "Set password" : "Sign in"}
        </button>
        {setup && <p className="muted small">At least 8 characters. You can change it later in Settings.</p>}
      </form>
    </div>
  );
}

function Shell({ session, reload }) {
  const [route, setRoute] = useState(parseRoute);
  const [inventory, setInventory] = useState({ hosts: [], vms: [], loaded: false, offline: false });
  const [groups, setGroups] = useState([]);
  const [notice, setNotice] = useState(null);
  const [bar, setBar] = useState(null);
  const [captureSlot, setCaptureSlot] = useState(null);
  const noticeTimer = useRef();

  const notify = useCallback((message, error = false) => {
    setNotice({ message, error });
    clearTimeout(noticeTimer.current);
    noticeTimer.current = setTimeout(() => setNotice(null), error ? 15000 : 6000);
  }, []);

  const refresh = useCallback(async () => {
    try {
      const data = await api("/api/vms");
      // Keep the same object while nothing changed, so tiles do not re-render every poll.
      setInventory((old) => {
        const next = { hosts: data.hosts, vms: data.vms, loaded: true, offline: false };
        const strip = (value) => JSON.stringify({ ...value, vms: value.vms.map(({ displays, ...vm }) => vm) });
        return old.loaded && !old.offline && strip(old) === strip(next) ? old : next;
      });
    } catch (error) {
      if (error.status !== 401) setInventory((old) => ({ ...old, offline: true }));
    }
  }, []);
  const refreshGroups = useCallback(async () => {
    try {
      const data = await api("/api/groups");
      setGroups((old) => (JSON.stringify(old) === JSON.stringify(data.groups) ? old : data.groups));
    } catch {
      // The inventory poll already reports a lost connection.
    }
  }, []);

  useEffect(() => {
    refresh();
    refreshGroups();
    const a = setInterval(refresh, 10000),
      b = setInterval(refreshGroups, 10000);
    const hash = () => setRoute(parseRoute());
    window.addEventListener("hashchange", hash);
    return () => {
      clearInterval(a);
      clearInterval(b);
      window.removeEventListener("hashchange", hash);
    };
  }, [refresh, refreshGroups]);

  async function signOut() {
    await api("/api/logout", { method: "POST" }).catch(() => {});
    reload();
  }

  const running = inventory.vms.filter((vm) => vm.state === "running").length;
  const down = inventory.hosts.filter((host) => host.enabled && !host.ok);
  const shell = { session, reload, inventory, refresh, groups, setGroups, notify, bar, captureSlot };
  return (
    <ShellContext.Provider value={shell}><BandwidthProvider>
      <header className="top">
        <div className="brand">VM Watcher</div>
        <nav>
          <a className={route.view === "wall" ? "chosen" : ""} href="#/">
            Wall
          </a>
          <a className={route.view === "control" ? "chosen" : ""} href="#/control">
            Take Control
          </a>
          <a className={route.view === "settings" ? "chosen" : ""} href="#/settings">
            Settings
          </a>
        </nav>
        <div className="bar" ref={setBar} />
        <Bandwidth />
        <span
          id="connection"
          className={inventory.offline || down.length ? "warn" : ""}
          title={down.map((host) => host.name + ": " + host.error).join("\n") || "All hosts reachable"}
        >
          {inventory.offline
            ? "Disconnected · retrying"
            : `${running} running / ${inventory.vms.length}` +
              (inventory.hosts.length > 1 ? ` · ${inventory.hosts.length} hosts` : "") +
              (down.length ? ` · ${down.length} unreachable` : "")}
        </span>
        <div className="header-actions">
          <span ref={setCaptureSlot} />
          <button onClick={signOut} title="Sign out of this browser">
            Sign out
          </button>
        </div>
      </header>
      {notice && (
        <div id="notice" role="status" className={notice.error ? "error" : ""}>
          {notice.message}
        </div>
      )}
      {route.view === "wall" && <Wall />}
      {route.view === "control" && <Control vmKey={route.key} />}
      {route.view === "settings" && <Settings />}
    </BandwidthProvider></ShellContext.Provider>
  );
}
