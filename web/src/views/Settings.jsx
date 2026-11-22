import { useCallback, useEffect, useState } from "react";
import { useShell } from "../App.jsx";
import { api } from "../lib/api.js";

const typeLabel = { local: "This machine", agent: "Agent", libvirt: "libvirt remote", demo: "Demo" };
const typeHelp = {
  local: "The hypervisor running on the same machine as this service. Everything is supported.",
  agent:
    "Connect to a VM Watcher agent that serves virtual machines or physical computers. Physical sources use a capture plugin and can use a keyboard and mouse plugin.",
  libvirt:
    "A remote libvirt connection with nothing installed on the hypervisor. Previews, control and audits work. Live consoles and audio only work for VMs whose VNC console listens on a network address this service can reach.",
};

export function Settings() {
  const { notify, reload, inventory, refresh } = useShell();
  const [view, setView] = useState(null);
  const [editing, setEditing] = useState(null); // null, "new", or a host id
  const load = useCallback(async () => {
    try {
      setView(await api("/api/settings"));
    } catch (error) {
      notify(error.message, true);
    }
  }, [notify]);
  useEffect(() => {
    load();
  }, [load]);
  if (!view)
    return (
      <main className="page">
        <p className="muted">Loading settings…</p>
      </main>
    );
  const changed = async () => {
    setEditing(null);
    await Promise.all([load(), refresh()]);
  };
  return (
    <main className="page">
      <section className="panel">
        <div className="panel-top">
          <h2>Hosts</h2>
          <button className="primary" onClick={() => setEditing("new")} disabled={editing === "new"}>
            Add host
          </button>
        </div>
        <p className="muted">
          Connect to an agent for virtual machines or physical computers, or connect directly to a remote libvirt host.
        </p>
        <table className="hosts">
          <thead>
            <tr>
              <th>Name</th>
              <th>Type</th>
              <th>Address</th>
              <th>Status</th>
              <th />
            </tr>
          </thead>
          <tbody>
            {view.hosts.map((host) => {
              const state = inventory.hosts.find((item) => item.id === host.id);
              return (
                <tr key={host.id} className={host.enabled ? "" : "disabled"}>
                  <td>
                    <strong>{host.name}</strong>
                  </td>
                  <td>{typeLabel[host.type] || host.type}</td>
                  <td className="mono">{host.type === "agent" ? host.url : host.type === "libvirt" ? host.uri : "—"}</td>
                  <td>
                    {!host.enabled ? (
                      <span className="muted">Disabled</span>
                    ) : !state ? (
                      <span className="muted">Checking…</span>
                    ) : state.ok ? (
                      <span className="status-ok">Connected · {state.count} VMs</span>
                    ) : (
                      <span className="status-bad">{state.error}</span>
                    )}
                  </td>
                  <td className="row-actions">
                    <button onClick={() => setEditing(host.id)}>Edit</button>
                  </td>
                </tr>
              );
            })}
            {!view.hosts.length && (
              <tr>
                <td colSpan={5} className="muted">
                  No hosts yet. Add the first one to see its VMs on the wall.
                </td>
              </tr>
            )}
          </tbody>
        </table>
        {editing && (
          <HostForm
            key={editing}
            host={view.hosts.find((host) => host.id === editing) || null}
            view={view}
            done={changed}
            cancel={() => setEditing(null)}
          />
        )}
      </section>

      <ViewerSettings view={view} saved={async () => Promise.all([load(), reload()])} />
      <PasswordForm />

      <section className="panel">
        <h2>About</h2>
        <p className="muted">
          VM Watcher {view.server.version} on {view.server.hostname}, listening on {view.server.bind}:{view.server.port}
          . {view.libvirt.available ? "libvirt is available on this machine." : view.libvirt.problem + "."}
        </p>
        <UpdateStatus initial={view.updates} />
      </section>
    </main>
  );
}

function HostForm({ host, view, done, cancel }) {
  const { notify } = useShell();
  const localListed = view.hosts.some((item) => item.type === "local");
  const [form, setForm] = useState(() => ({
    name: host?.name || "",
    type: host?.type || "agent",
    url: host?.url || "http://",
    uri: host?.uri || "qemu+ssh://root@",
    token: "",
    enabled: host?.enabled ?? true,
  }));
  const [test, setTest] = useState(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const set = (field) => (event) => {
    setForm({ ...form, [field]: event.target.type === "checkbox" ? event.target.checked : event.target.value });
    setTest(null);
  };
  const body = () => ({ ...form, id: host?.id });
  async function run(action) {
    setBusy(true);
    setError("");
    try {
      await action();
    } catch (failure) {
      setError(failure.message);
    }
    setBusy(false);
  }
  const save = (event) => {
    event.preventDefault();
    run(async () => {
      await api(host ? "/api/hosts/" + host.id : "/api/hosts", { method: host ? "PUT" : "POST", body: body() });
      notify(`Host “${form.name}” saved.`);
      await done();
    });
  };
  const probe = () => run(async () => setTest(await api("/api/hosts/test", { method: "POST", body: body() })));
  const remove = () => {
    if (!confirm(`Remove “${host.name}”? Its VMs leave the wall and any groups; saved audits are kept.`)) return;
    run(async () => {
      await api("/api/hosts/" + host.id, { method: "DELETE" });
      notify(`Host “${host.name}” removed.`);
      await done();
    });
  };
  return (
    <form className="host-form" onSubmit={save}>
      <h3>{host ? `Edit ${host.name}` : "Add a host"}</h3>
      <div className="fields">
        <label>
          Name
          <input required maxLength={60} value={form.name} onChange={set("name")} placeholder="e.g. Rack B hypervisor" />
        </label>
        <label>
          Type
          <select value={form.type} onChange={set("type")} disabled={!!host}>
            {(host?.type === "local" || (view.libvirt.available && !localListed)) && <option value="local">This machine</option>}
            <option value="agent">Agent</option>
            <option value="libvirt">libvirt remote (limited)</option>
            {host?.type === "demo" && <option value="demo">Demo</option>}
          </select>
        </label>
        {form.type === "agent" && (
          <>
            <label>
              Agent address
              <input required value={form.url} onChange={set("url")} placeholder="http://192.0.2.4:6089" />
            </label>
            <label>
              Agent token
              <input
                value={form.token}
                onChange={set("token")}
                required={!host?.hasToken}
                autoComplete="off"
                spellCheck={false}
                placeholder={host?.hasToken ? "Leave blank to keep the saved token" : "64 characters"}
              />
            </label>
          </>
        )}
        {form.type === "libvirt" && (
          <label className="wide">
            libvirt URI
            <input required value={form.uri} onChange={set("uri")} placeholder="qemu+ssh://root@192.0.2.4/system" />
          </label>
        )}
        <label className="check">
          <input type="checkbox" checked={form.enabled} onChange={set("enabled")} /> Enabled
        </label>
      </div>
      <p className="muted">{typeHelp[form.type]}</p>
      {form.type === "agent" && (
        <details>
          <summary>How to install the agent</summary>
          <p className="muted">
            Run <code>vmwatcher</code> on the host using a private address the viewer can reach. For physical computers,
            place <code>physical.json</code> in the agent data directory before starting it.
          </p>
          <pre>vmwatcher agent --bind HOST_ADDRESS --port 6089 --data /var/lib/vmwatcher-agent</pre>
          <p className="muted">Enter the agent URL above, then run this on the agent host to get its token.</p>
          <pre>vmwatcher agent-token --data /var/lib/vmwatcher-agent</pre>
          <p className="muted">
            The agent only answers requests that carry this token. Traffic between this service and the agent is plain
            HTTP, so keep both on a network you trust.
          </p>
        </details>
      )}
      {form.type === "libvirt" && (
        <p className="muted">
          SSH connections use the keys of the account this service runs as; the host must already be in its
          known_hosts.
        </p>
      )}
      {test && (
        <p className={test.ok ? "status-ok" : "form-error"} role="status">
          {test.ok ? `Connected: ${test.vms} VMs, ${test.running} running.` : `Could not connect: ${test.error}`}
        </p>
      )}
      {error && (
        <p className="form-error" role="alert">
          {error}
        </p>
      )}
      <div className="form-actions">
        <button className="primary" disabled={busy}>
          Save host
        </button>
        <button type="button" onClick={probe} disabled={busy}>
          Test connection
        </button>
        <button type="button" onClick={cancel}>
          Cancel
        </button>
        {host && (
          <button type="button" className="danger push" onClick={remove} disabled={busy}>
            Remove host
          </button>
        )}
      </div>
    </form>
  );
}

function ViewerSettings({ view, saved }) {
  const { notify } = useShell();
  const [form, setForm] = useState(() => ({ ...view.settings, allowedHosts: view.settings.allowedHosts.join(", ") }));
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const text = (field) => ({ value: form[field], onChange: (e) => setForm({ ...form, [field]: e.target.value }) });
  const number = (field, min, max) => ({
    type: "number",
    min,
    max,
    required: true,
    value: form[field],
    onChange: (e) => setForm({ ...form, [field]: e.target.value }),
  });
  async function save(event) {
    event.preventDefault();
    setBusy(true);
    setError("");
    try {
      await api("/api/settings", {
        method: "PUT",
        body: {
          videoEncoder: form.videoEncoder,
          controlEnabled: form.controlEnabled,
          sessionHours: Number(form.sessionHours),
          previewSeconds: Number(form.previewSeconds),
          maxLivePerSession: Number(form.maxLivePerSession),
          maxLiveTotal: Number(form.maxLiveTotal),
          allowedHosts: form.allowedHosts.split(/[\s,]+/).filter(Boolean),
        },
      });
      await saved();
      notify("Settings saved.");
    } catch (failure) {
      setError(failure.message);
    }
    setBusy(false);
  }
  return (
    <section className="panel">
      <h2>Viewer</h2>
      <form onSubmit={save}>
        <div className="fields">
          <label>
            Default preview refresh (seconds, 0 pauses)
            <input {...number("previewSeconds", 0, 300)} />
          </label>
          <label>
            Stay signed in while idle (hours)
            <input {...number("sessionHours", 168, 720)} />
          </label>
          <label>
            Live video encoder
            <select {...text("videoEncoder")}>
              {Object.entries(view.videoEncoders).map(([id, label]) => <option key={id} value={id}>{label}</option>)}
            </select>
          </label>
          <label>
            Live consoles per browser
            <input {...number("maxLivePerSession", 1, 16)} />
          </label>
          <label>
            Live consoles in total
            <input {...number("maxLiveTotal", 1, 64)} />
          </label>
          <label className="wide">
            Extra addresses this viewer may be opened through
            <input {...text("allowedHosts")} placeholder="e.g. vmwall.lan, 10.0.0.5" />
          </label>
          <label className="check wide">
            <input
              type="checkbox"
              checked={form.controlEnabled}
              onChange={(e) => setForm({ ...form, controlEnabled: e.target.checked })}
            />{" "}
            Allow keyboard and mouse control of VMs (Take Control)
          </label>
        </div>
        <p className="muted">
          The viewer always answers on the address it is bound to and on localhost. Requests that arrive under any
          other name are refused unless the name is listed above.
        </p>
        {error && (
          <p className="form-error" role="alert">
            {error}
          </p>
        )}
        <div className="form-actions">
          <button className="primary" disabled={busy}>
            Save settings
          </button>
        </div>
      </form>
    </section>
  );
}

function PasswordForm() {
  const { notify } = useShell();
  const [form, setForm] = useState({ current: "", password: "", again: "" });
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const field = (name) => ({
    type: "password",
    required: true,
    value: form[name],
    onChange: (e) => setForm({ ...form, [name]: e.target.value }),
  });
  async function save(event) {
    event.preventDefault();
    if (form.password !== form.again) return setError("The two new passwords do not match");
    setBusy(true);
    setError("");
    try {
      await api("/api/password", { method: "POST", body: { current: form.current, password: form.password } });
      setForm({ current: "", password: "", again: "" });
      notify("Password changed. Other browsers have been signed out.");
    } catch (failure) {
      setError(failure.message);
    }
    setBusy(false);
  }
  return (
    <section className="panel">
      <h2>Password</h2>
      <p className="muted">One shared password opens this viewer. Changing it signs out every other browser.</p>
      <form onSubmit={save}>
        <div className="fields">
          <label>
            Current password
            <input autoComplete="current-password" {...field("current")} />
          </label>
          <label>
            New password
            <input autoComplete="new-password" minLength={8} {...field("password")} />
          </label>
          <label>
            Repeat new password
            <input autoComplete="new-password" {...field("again")} />
          </label>
        </div>
        {error && (
          <p className="form-error" role="alert">
            {error}
          </p>
        )}
        <div className="form-actions">
          <button className="primary" disabled={busy}>
            Change password
          </button>
        </div>
      </form>
    </section>
  );
}

function UpdateStatus({ initial }) {
  const [info, setInfo] = useState(initial);
  const [error, setError] = useState("");
  useEffect(() => {
    if (!info?.checking) return;
    const timer = setTimeout(() => {
      api("/api/updates").then(setInfo).catch((e) => setError(e.message));
    }, 1500);
    return () => clearTimeout(timer);
  }, [info]);
  if (!info) return null;
  async function check() {
    setError("");
    try { setInfo(await api("/api/updates", { method: "POST" })); }
    catch (e) { setError(e.message); }
  }
  return <>
    <p role="status">{error || (info.checking ? "Checking GitHub…" : info.message)}</p>
    <button onClick={check} disabled={info.checking && !error}>Check for updates</button>
  </>;
}
