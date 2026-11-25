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

### Physical computers on the wall

A capture card and a keyboard/mouse emulator can drive a wall tile without
libvirt. On the Linux host with the capture device, put `physical.json` in its agent
data directory (`<install-root>/agent-data` for the systemd install) **before**
starting the agent. Use a stable ID from `uuidgen` for each computer.

```json
{
  "sources": [
    {
      "id": "a94d4344-3634-46cf-a90f-f7fa7fec48cf",
      "name": "Physical computer",
      "capture": "/opt/vmwatcher/plugins/capture",
      "hid": "/opt/vmwatcher/plugins/hid"
    }
  ]
}
```

The file selects physical-source mode for that agent. `capture` is one plugin
path, or an array of up to four paths, one per display of that computer.
Live video streams the first display; the others refresh once a second, as
for multi-monitor VMs, and previews show all of them side by side. Use a separate agent instance if the same host should also list
libvirt guests. Restart the agent after changing the file.

Install or update the agent with
`scripts/deploy.sh agent <capture-host> <private-address>`, then add the agent
URL and token under **Settings → Hosts** in an updated VM Watcher viewer.
Physical computers support previews, encoded live video, and audits. When the
viewer uses VNC for VMs, physical tiles use software H.264. Audio is unavailable
for physical sources. Omit `hid` for a view-only tile. `hid` is either the path
of a HID plugin (below) or an object naming a device the agent drives itself.
Either way, input also requires **Settings → Keyboard and mouse control**.

#### ESP32-S2 keyboard/mouse dongle

The current setup, measurements and procedures for the physical test PC are in
[docs/physical-pc.md](docs/physical-pc.md).

The agent drives the ESP32-S2 "Mordin KVM" dongle natively. The dongle is a USB
keyboard and relative mouse on the controlled computer and takes commands over
TCP 8889 after an HMAC-SHA256 challenge:

```json
"hid": {
  "type": "esp32-s2-kvm",
  "address": "192.168.1.98",
  "port": 8889,
  "keyFile": "/opt/vmwatcher/agent-data/s2-kvm.key",
  "screen": [1024, 768],
  "pointer": { "model": "divided", "divisor": 8, "step": 12, "small": 3 },
  "displays": [[0, 0, 1024, 768], [1024, 0, 1024, 768]]
}
```

- `keyFile` holds the dongle's 64-character lowercase hex key, mode 0600.
  Keep it inside the agent's data directory: the systemd unit hides `/root`
  and `/home` from the agent.
- `address` is the dongle's DHCP address; give it a reservation so it stays put.
- The agent keeps one authenticated session open. The dongle serves one client
  at a time, so other tools (`kvmctl.py`) wait until the agent closes its idle
  session, at most 45 s after the last input.
- The dongle releases every held input on disconnect and after three seconds
  without a report. The agent refreshes held keys and buttons once a second,
  and releases them itself when the viewer has sent nothing for 30 s.
- The mouse is relative only. The agent pins the pointer to the top-left corner
  with large negative moves, then tracks where it is and sends the reports
  that reach each new position. How a computer turns counts into pointer
  movement differs, so `pointer` describes it, in `screen` coordinates:
  - `"model": "linear"` (default): the pointer moves `mickeysPerPixel` counts
    per pixel (default 1). Set `screen` to the computer's resolution, which
    the capture card cannot report, and turn pointer acceleration off on the
    computer (Windows "Enhance pointer precision", desktop acceleration).
    `rehomeOnClick` (default true here) starts each new press from the corner
    again, so clicks land where shown despite drift, with a visible jump.
  - `"model": "divided"`: each report moves `step` pixels for every whole
    `divisor` counts and `small` pixels when it is nonzero but below the
    divisor. Measured on the ASUS PRIME Z270-P UEFI setup (AMI): `screen`
    [1024, 768], `divisor` 8, `step` 12, `small` 3. The pointer then lands
    within 3 of those pixels of the target, with no drift over long drags.
    `rehomeOnClick` defaults to false.
  - The settings fit one environment. A computer that boots from UEFI setup
    into an OS with a different pointer response needs `physical.json` changed
    and the agent restarted.
- `displays` places each display in the pointer space as
  `[x, y, width, height]`, in the same order as `capture`. Without it there is
  one display covering `screen`. Homing goes to the top-left of all displays,
  so they should share a top edge. Input that switches to another display
  re-homes first, which keeps tracking right when a display is not shown
  (UEFI setup drives only one output).
- QEMU key names from the viewer are mapped to USB HID keyboard usages; keys
  without a mapping are ignored.
- The session is authenticated but not encrypted. Keep the dongle and the agent
  on the management network.

#### Device plugin contract

Both paths point to administrator-installed executables. The agent launches
them directly with no arguments under its service account. Keep the
executables and configuration writable only by the administrator. Give the
service account access to the capture card and HID emulator. Put plugins
outside home directories when using the supplied systemd unit.

- **Capture** writes a single RGB PNG or binary P6 PPM to stdout and exits
  successfully. Stdin is empty. For a Linux V4L2 capture card, a starting
  point for the executable is below. Set the device path and capture format
  for your hardware.
- **HID** reads one JSON object from stdin through EOF, applies events in the
  order supplied, and exits successfully with empty stdout. `head` is always
  `0`. An `abs` event gives an `x` or `y` coordinate from 0 to 32767 across
  the display. A `btn` event gives a `button` and boolean `down`; buttons are
  `left`, `middle`, `right`, `wheel-up`, and `wheel-down`. A `key` event
  gives a QEMU qcode and boolean `down`. The plugin maps these to the emulator
  protocol and preserves held keys, buttons and pointer position across
  invocations.
- Report diagnostics on stderr and return a nonzero status on failure.
  Each call has a three-second deadline. For hardware that needs a persistent
  capture or input connection, use an executable that talks to a local device
  process over IPC. Keep the agent on a private network reachable by the
  viewer.

```sh
#!/bin/sh
exec /usr/bin/ffmpeg -nostdin -hide_banner -loglevel error \
  -f v4l2 -i /dev/video0 -frames:v 1 -pix_fmt rgb24 -threads 1 -c:v png -f image2pipe pipe:1
```

```json
{"head":0,"events":[{"type":"abs","data":{"axis":"x","value":16384}},{"type":"key","data":{"key":{"type":"qcode","data":"a"},"down":true}}]}
```

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

The first visit asks for the shared password. The server stores session hashes
and expiry times in `sessions.json` in its data directory so valid sessions
survive a restart. To reset a forgotten one:
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
and offers an on-demand check. The daemon reads public tag references over HTTPS without credentials.
Deploy a new version using the installer.
