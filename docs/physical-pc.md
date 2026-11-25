# Controlling the physical PC

How VM Watcher sees and drives the physical test computer ("Physical PC" on
the wall). Facts below were checked on 2026-11-26 unless marked otherwise.

## Hardware and wiring

| Part | Where | Details |
| --- | --- | --- |
| Test computer | on the bench | ASUS PRIME Z270-P, AMI BIOS 0810, Pentium G4400, 32 GB, M.2 Micron 256 GB + SanDisk 256 GB, both with Windows Boot Manager |
| Video, display 1 | test PC HDMI → Elgato Game Capture HD60 X on **mordin** USB 3 | `/dev/v4l/by-id/usb-Elgato_Game_Capture_HD60_X_MX20L1A13493-video-index0` (`/dev/video0`); shows an "elgato / No Signal" card without input |
| Video, display 2 | test PC second output → MACROSILICON USB3.0 Video stick on **mordin** | `/dev/v4l/by-id/usb-MACROSILICON_USB3.0_Video_35720661-video-index0` (`/dev/video2`); MJPEG or YUYV up to 1920×1080; plain black without a picture |
| Keyboard and mouse | ESP32-S2 Mini (LOLIN S2 Mini) USB-C → test PC | USB HID "HomeLab Mordin KVM Keyboard & Mouse"; Wi-Fi `Nowlive Mesh 2.4g`, hostname `mordin-kvm`, last DHCP address **192.168.1.98**, TCP **8889** |
| Agent | **mordin** (192.168.88.4, Debian, libvirt host) | `vmwatcher-agent.service` on `http://192.168.88.4:6089`, data in `/opt/vmwatcher/agent-data` |
| Viewer | **EDI** | `https://192.168.88.3:6443`, host `mordin` in Settings → Hosts |

Also on mordin, not used by this setup: a CP2102 serial bridge that was
reconnecting every second on 2026-11-26 00:58. The kernel also logs
`uvcvideo: Non-zero status (-71)` bursts from a capture device.

The older Legion route (`/opt/vmwatcher/plugins/legion-hid`,
`legion_control.py`, ESP32 receiver at 192.168.1.152:8889, keyboard taps only,
no mouse) is retired. Its files remain on mordin; its config is saved as
`agent-data/physical.json.legion-20261126`.

## Access

| What | How |
| --- | --- |
| mordin | `ssh mordin`. Read `/root/AGENTS.md` there first: no Docker, keep the host vanilla |
| Dongle firmware, client and notes | `ssh dev` (192.168.0.8), `/root/.t3/projects/homelab/dongle.md` and `/root/.t3/projects/homelab/kvm/s2-mini/` (`src/main.cpp`, `kvmctl.py`, `README.md`, PlatformIO env `s2-mini`) |
| Dongle key | `/root/mordin-kvm-auth.key` on mordin (original); `/opt/vmwatcher/agent-data/s2-kvm.key` (copy the agent reads, mode 0600). 64 lowercase hex characters. Never print or commit it |
| Agent token | `/opt/vmwatcher/agent-data/agent.json` on mordin; also stored in the viewer's host entry |

Anything the dongle accepts except `ping` reaches the real computer. Moving
the pointer is harmless; do not send keys or clicks without the user asking.
In UEFI setup a click or key can change firmware settings.

## Agent configuration on mordin

`/opt/vmwatcher/agent-data/physical.json`, read when the agent starts
(`systemctl restart vmwatcher-agent` after a change):

```json
{
  "sources": [
    {
      "id": "5c7c260e-ff75-4dd6-b941-a53463e1fede",
      "name": "Physical PC",
      "capture": [
        "/opt/vmwatcher/plugins/elgato-hd60x-capture",
        "/opt/vmwatcher/plugins/macrosilicon-capture"
      ],
      "hid": {
        "type": "esp32-s2-kvm",
        "address": "192.168.1.98",
        "port": 8889,
        "keyFile": "/opt/vmwatcher/agent-data/s2-kvm.key",
        "screen": [1024, 768],
        "rehomeOnClick": false,
        "pointer": { "model": "divided", "divisor": 8, "step": 12, "small": 3 },
        "displays": [[0, 0, 1024, 768], [1024, 0, 1024, 768]]
      }
    }
  ]
}
```

The pointer settings above fit the **UEFI setup**. See
[Pointer behaviour](#pointer-behaviour) before using it in Windows.

Each capture plugin grabs one 1280×720 frame with ffmpeg and returns a PNG:
NV12 from the Elgato (about 0.2 s), MJPEG from the MACROSILICON stick (about
0.35–0.85 s). Live video streams display 1; display 2 refreshes once a second
in Take Control and the wall preview shows both side by side. The card can deliver up to 3840×2160 NV12 or
1920×1080 YUYV, but it cannot report the test PC's own output resolution
(`v4l2-ctl` is not installed on mordin).

The agent's systemd unit hides `/root` and `/home` (`ProtectHome=true`), so
the key must live inside `agent-data`.

Deploy agent code changes with `scripts/deploy.sh agent mordin 192.168.88.4`
from this repository. It builds on mordin with its own g++, runs the tests
there and restarts the service.

## Dongle protocol

From the firmware (`kvm/s2-mini/src/main.cpp` on dev):

1. Connect to TCP 8889. The dongle sends `CHALLENGE <32 lowercase hex>\r\n`.
2. Reply `auth <hex HMAC-SHA256(key, challenge)>\n`. The key is the **ASCII
   text** of the 64 hex characters, not the decoded bytes, and the message is
   the 32 hex characters as text. Answer: `OK`; anything wrong gets `ERR` and
   the connection closes. Authentication must finish within 3 s.
3. Send one command per line; each gets one reply line, `OK…` or `ERR`.

| Command | Effect |
| --- | --- |
| `ping` | `OK Mordin KVM` |
| `down <usage>` / `up <usage>` | press or release a USB HID keyboard usage, decimal, 4–164 or 224–231 |
| `mouse <dx> <dy> <wheel> <pan> <buttons>` | relative report, each value −127…127; buttons bitmask left 1, right 2, middle 4 (sets the held state) |
| `repeat <usage> <interval-ms> <duration-ms>` | tap a key repeatedly now (interval 50–2000, duration ≤ 30000) |
| `boot-once …` / `boot …` / `boot-off` | arm a repeat for the next / every dongle start (e.g. F12 = 69 for a boot menu); clear them |
| `release` | stop a repeat, release every key and button |

Behaviour that matters:

- **One client at a time.** A second connection waits until the first closes.
- **Disconnect releases everything**, and so do **3 s without an HID report**.
  To hold a key across time, stay connected and keep sending reports.
- An authenticated session is dropped after **60 s** without a command.
- The TCP stream is authenticated but **not encrypted**; keep it on the
  management network.
- Wi-Fi and the key are provisioned over the S2's USB CDC port (115200 baud,
  `wifi <ssid hex> <password hex or -> <key>`). That port is on the test PC's
  side, so reflashing or re-provisioning needs the board plugged into another
  machine. Holding the board's `0` button allows replacing the key.

Manual check from mordin, which sends no input:

```sh
python3 kvmctl.py 192.168.1.98 --key-file /root/mordin-kvm-auth.key ping
```

(`kvmctl.py` is on dev; copy it over if needed.) The agent holds the dongle's
single session while the viewer is in use and closes it 45 s after the last
input, so manual tools may have to wait that long.

## How the agent drives it

Native driver `core/src/kvm.cpp`, selected by an object-valued `hid` in
`physical.json`:

- One persistent authenticated session, opened on the first input.
- Keys: QEMU key names from the viewer are mapped to HID usages (`a`→4,
  `ret`→40, `f1`→58, `shift`→225, `meta_l`→227, …); unmapped keys are skipped.
- Held keys and buttons are refreshed with `mouse 0 0 0 0 <buttons>` every
  1.5 s so the dongle's 3 s release does not drop them; they are released
  after 30 s without input from the viewer.
- Wheel: one `mouse 0 0 ±1 0 <buttons>` per notch.
- Pointer: the viewer sends absolute positions (0–32767). The agent homes the
  pointer to the top-left corner with `mouse -127 -127 …` reports (screen
  edges clamp it), tracks its position in `screen` coordinates, and sends the
  reports that reach each new target. Moves are pipelined 16 lines at a time.

### Two displays

`hid.displays` places the displays in the pointer space, in `capture` order.
The current layout assumes display 2 sits to the right of display 1, same
size, tops aligned. It has not been checked against Windows' display
arrangement; fix it once the PC runs an OS with both outputs active. Input on
a different display than the last re-homes first. In UEFI setup only one
output is active, so pointer input on display 2 there just pins the pointer
to the right edge of display 1 until the next re-home.

## Pointer behaviour

The dongle is a **relative** mouse, so where the pointer ends up depends on
how the computer turns counts into movement.

### Measured on the UEFI setup (ASUS PRIME Z270-P, AMI)

The firmware works in a **1024×768** space, scaled to the screen, and ignores
the size of small reports:

| Counts in one report (per axis) | Pointer moves |
| --- | --- |
| 0 | 0 |
| 1–7 | 3 px |
| 8 or more | 12 px × ⌊counts / 8⌋ |

- X and Y are independent and follow the same rule; negative moves are
  symmetric. On the 1280×720 capture that is 3.75 px / 15 px in X and
  2.8 px / 11.2 px in Y.
- Spacing reports out (30 ms) changes nothing: no reports are dropped.
- Treating counts as pixels (the first driver) put the pointer up to hundreds
  of pixels off.

Results with `"model": "divided"` (divisor 8, step 12, small 3, screen
1024×768): seven absolute targets landed within 4 capture px, a revisit
landed on the same spot, and 120 small streamed updates over three legs
ended 2 px from the target. Resolution is 3 BIOS pixels.

### Windows and other systems: not measured yet

Windows uses its own pointer speed and acceleration ("Enhance pointer
precision"), so the UEFI settings will be wrong there. To use the PC in an
OS, either:

- measure it as described below and set `pointer` to match, or
- switch to `"model": "linear"` with `screen` set to the OS resolution,
  acceleration turned off in the OS, `mickeysPerPixel` tuned (1 at the default
  Windows speed with acceleration off), and `rehomeOnClick: true` so each
  click re-homes and absorbs drift (visible jump to the corner).

Either way `physical.json` holds one profile at a time; moving between UEFI
setup and an OS means editing it and restarting the agent. Options for
automatic switching: a calibration step in the viewer, or absolute-pointer
(tablet) support in the dongle firmware, which Windows uses exactly while
the UEFI setup would likely still need relative mode.

### How to measure a pointer response

The method used for the table above:

1. Stop using the viewer for 45 s (or stop the agent) so the dongle's single
   session is free, then connect directly from mordin.
2. Home: send about 25 × `mouse -127 -127 0 0 0`; capture a reference frame.
3. For each step size *s*: home, optionally move to a starting point away
   from animated areas, send *n* reports of `mouse s 0 0 0 0` (then the same
   for Y), wait 0.4 s, capture.
4. Find the pointer by diffing each frame against the reference in 4 px cells
   and taking the largest changed cluster, after masking regions that change
   on their own. On the UEFI EZ Mode screen those are the clock (top), the
   CPU temperature graph, the spinning fan icons, the animated background
   along the top and bottom edges and the right-hand panel's top.
5. Movement per report = (tip − start) / *n*. Check a few frames by eye.

Captures come from the agent: `GET /agent/v1/vms/<id>/heads/0.png?fresh=1`
with `Authorization: Bearer <agent token>`. Pointer moves for an end-to-end
check go to `POST /agent/v1/vms/<id>/input` with
`{"head":0,"events":[{"type":"abs","data":{"axis":"x","value":16384}}, …]}`.

## Last known state (2026-11-26)

- 01:51: after the second display was configured, **neither card had a
  picture** (Elgato "No Signal", MACROSILICON black) while the dongle still
  answered on 8889. The PC's outputs were off or switching; nothing was sent
  to wake it.

- The test PC was in **UEFI setup, EZ Mode**, after a "Press F1 to Run SETUP"
  prompt about RAID/Optane settings. The BIOS listed "1 Keyboard, 1 Mouse"
  (the dongle).
- Keyboard input through the agent has not yet been tried on the real PC.
- Agent 2.1.0 with the divided pointer model is running on mordin.
