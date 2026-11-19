<img src="assets/icon.svg" width="64" height="64" alt="">

# VM Watcher

A wall of libvirt virtual machines across several hypervisor hosts: previews,
live view-only consoles, audio, multi-monitor views, keyboard/mouse control and
saved display audits, behind a shared password.

```
browser ──HTTPS/WebSocket─> central service ──┬─ libvirt on the same machine
 (React)                    (vmwatcher serve) ├─ agent on another host  (vmwatcher agent)   full support
                                              └─ remote libvirt URI     (qemu+ssh://…)      limited
```

One central service holds the configuration (hosts, settings, password, groups,
audits) and is the only thing browsers talk to. Each hypervisor is reached in
one of three ways, chosen per host in **Settings**:

| Host type      | Install on the hypervisor | Previews, audits, control | Live console and audio |
| -------------- | ------------------------- | ------------------------- | ---------------------- |
| This machine   | nothing (it is the central service) | yes | yes |
| Agent          | the `vmwatcher` binary    | yes                       | yes |
| libvirt remote | nothing                   | yes                       | only for VMs whose VNC console listens on an address the central service can reach |

## Layout

- `core/` — C++17, one binary (`vmwatcher`) for both the central service and
  the agent. No third-party build dependencies: libvirt and zlib are loaded at
  run time, HTTP/WebSocket/JSON/XML/PNG/crypto are in-tree. Builds with GCC 10+
  or Clang 12+ (`make -C core`), so it can be compiled on each host.
- `web/` — React + Vite front end (wall, Take Control, Settings, sign-in).
- `deploy/`, `scripts/deploy.sh`, `scripts/make-certs.sh` — systemd units, the
  installer and the internal certificate authority.
- `legacy/` — the previous single-host Node.js implementation.

## Develop

```
make -C core test                 # unit + central/agent integration on loopback
(cd web && npm ci && npm test && npm run build)

# Run everything on a machine without libvirt, with synthetic VMs:
core/build/vmwatcher agent --demo --port 16089 --data .work/demo/agent
core/build/vmwatcher serve --demo --port 16088 --data .work/demo/central --web web/dist
(cd web && VMWATCHER_DEV_API=http://127.0.0.1:16088 npm run dev)   # optional hot reload
```

## Install

```
scripts/deploy.sh central <ssh-host> <bind-address> [port] [install-root]
scripts/deploy.sh agent <ssh-host> <address>   # another hypervisor; prints the token to paste into Settings
```

The script tests locally, compiles on the target with its own `g++`, installs a
sandboxed systemd unit and restarts it. The central service uses its own systemd unit and data directory. To bring the
old wall's groups across, copy its `data/groups.json` into the new `data/`
directory (old entries are attributed to the host called `local`).

### HTTPS

The central service serves HTTPS only (`--tls-cert`, `--tls-key`; TLS 1.2+).
The first install creates an internal certificate authority and a server
certificate in `<install-root>/tls` (`scripts/make-certs.sh`):

- Import `tls/ca.crt` into each browser, or
- have your existing internal CA sign `tls/server.csr` and write the result plus
  that CA's chain to `tls/server.pem`, then restart the service.

Server certificates last 397 days; re-run `make-certs.sh` with the same
directory and names to renew. Without the two TLS flags the service speaks
plain HTTP, which is how the demo and the tests run.

The first visit asks for the shared password. To reset a forgotten one:
`echo 'new password' | vmwatcher passwd --data <data dir>`, then restart.

## Security model

- **One shared password** (PBKDF2-SHA256, 210 000 iterations) opens the viewer;
  everyone who has it can use Settings. Sessions are HttpOnly, SameSite=Strict
  cookies held in memory, so a service restart signs everyone out. Five wrong
  passwords lock an address out for a growing interval.
- **HTTPS between browsers and the central service**, with internal
  certificates; cookies are `Secure` and HSTS is sent. **Agents still speak
  plain HTTP** to the central service, so agent tokens and VM screens cross
  that link unencrypted: keep agents on a trusted network, and expose neither
  the viewer nor an agent publicly.
- The viewer only answers on its bind address, localhost and names listed in
  Settings; cross-site and cross-origin requests are refused, and every change
  must come from the viewer's own page.
- Consoles are proxied through a fail-closed read-only RFB filter: viewers can
  request frames but never send keys, pointer, clipboard, resize or power
  messages. Control goes through a separate endpoint that rebuilds every event
  from validated fields before it reaches QEMU, and can be switched off.
- On a hypervisor the proxy only dials that machine's own addresses. Agents
  answer nothing without their 256-bit token. Remote libvirt URIs are limited
  to plain `qemu+…` transports and options that cannot name a command.
- Tokens are never sent back to the browser. There are no VM lifecycle, disk or
  arbitrary proxy endpoints.

## Live video

In Settings → Viewer, select the live video encoder for primary displays on the
wall and control page. VNC remains the default. The other options encode on
the central service using FFmpeg, sampling primary displays at up to 8 fps
and sending fragmented MP4 over HTTPS. Software H.264 and H.265 and the
supported Radeon RX 580 VA-API codecs appear after a successful synthetic
encode at first use. Restart the service after changing drivers. Playback
uses the browser's native H.264 or HEVC decoder. Agent-side encoding and the
worker-based WASM/WebGL renderer are future work.

## Version checks

The central daemon checks stable GitHub tags of the form `vMAJOR.MINOR.PATCH`
at startup and every six hours. Settings → About shows the installed version
and offers an on-demand check. For private repositories, register the installed
service's `updates/github.pub` as a read-only GitHub deploy key. Checking
downloads tag references only. Deploy a new version using the installer.
