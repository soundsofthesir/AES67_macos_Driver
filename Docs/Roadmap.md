# Roadmap

> **Experimental.** Updated 19 November 2026.

A free, open-source AES67 driver for the Mac. The goal is to get a console's network audio onto a Mac without a licence, and to send the Mac's apps back out to the network as separate streams.

## Where it stands

The driver shows up in Core Audio as one device with 128 inputs and 128 outputs.

| Area | Status | Notes |
|---|---|---|
| Receive | Working | Fixed 8 ms latency. The device clock locks to the incoming stream and rides out short network stalls. Received a 1-channel stream from Riedel gear with no loss. |
| Transmit | First hardware test passed | Sends continuously on the device clock (or PTP time) and announces streams over SAP. A Riedel Artist intercom plays a 1-channel stream from the Mac. Not yet tried with Dante or RAVENNA, or with more than one channel. |
| PTP | Working on a real network, off by default | With `ptp.json` turning it on, the slave drives the device clock, receive streams are placed by PTP time, and transmitted streams are stamped with it and name the grandmaster. Locks to a real grandmaster in about 10 seconds with unicast Delay_Req. Only short runs so far ([#21](https://github.com/soundsofthesir/AES67_macos_Driver/issues/21)). |
| Manager app | Planned | Builds and runs on Apple Silicon and Intel, but doesn't control the driver yet ([#27](https://github.com/soundsofthesir/AES67_macos_Driver/issues/27), [#38](https://github.com/soundsofthesir/AES67_macos_Driver/issues/38)). Streams are set with `aes67ctl` or in `streams.json`. |
| Install | Pre-release | A signed and notarized `.pkg` on the [releases page](https://github.com/soundsofthesir/AES67_macos_Driver/releases), first published as 0.1.0 alpha 1 on 19 November 2026. It installs the driver, `aes67ctl` and the keepalive service. |

CI builds and tests every change on macOS (Release, and with AddressSanitizer and UndefinedBehaviorSanitizer) and on Linux.

## Steps

In order. Each step depends on the ones before it, so dates aren't promised.

### 1. PTP and testing with real devices (next)

Follow a PTP grandmaster on the network and timestamp sent audio against it. Then test with Dante devices in AES67 mode, RAVENNA devices and anything else people bring.

- Riedel Artist plays a stream sent from the Mac, with PTP on (done, 18 November 2026).
- Dante and RAVENNA devices play streams sent from the Mac.
- Stereo and 8-channel streams, and runs of several hours, on hardware.
- Several senders stay in sync with each other. The measured path delay moved between 62 and 141 µs from one lock to the next, so the servo issues [#192](https://github.com/soundsofthesir/AES67_macos_Driver/issues/192) to [#196](https://github.com/soundsofthesir/AES67_macos_Driver/issues/196) matter here.

Issues [#21](https://github.com/soundsofthesir/AES67_macos_Driver/issues/21), [#28](https://github.com/soundsofthesir/AES67_macos_Driver/issues/28). The design is in the [step 2 plan](Step2-Clocking-Plan.md).

### 2. Signed installer (done)

A signed and notarized download on the GitHub releases page, first published 19 November 2026. Installing it doesn't need Homebrew, a compiler or the command line.

### 3. Manager app: streams and diagnostics (planned)

The Manager app controls the driver directly. Streams announced on the network appear in a list and can be added with a click, without editing files or restarting Core Audio.

- PTP lock and offset, packet loss, audio that arrived but was written too late to play, and the network interface in use, shown live. The driver already counts late audio per stream.
- Plain warnings for common network problems: no IGMP querier on the switch, the AES67 interface in a bridge (Internet Sharing), Energy Efficient Ethernet, Wi-Fi, and streams outside Dante's 239.69.x.x range.

### 4. Per-app routing (planned)

Send each app's audio to its own stream. Set the AES67 device as the Mac's output, then choose which stream each app goes to. Apps that pick their own channels, like a DAW, keep doing so.

The driver can see which app each piece of audio comes from before Core Audio mixes them, so it can move each app onto its own channels. Every app is already on the driver's clock, so this needs no resampling and adds no latency.

Example for a service or live show, with up to 8 channels per stream:

| App | Use | Driver channels | Network |
|---|---|---|---|
| Zoom | Remote guest or presenter | 1-2 | Stream 1 |
| Spotify | Walk-in and interval music | 3-4 | Stream 2 |
| Chrome | Video playback from the web | 5-6 | Stream 3 |
| QLab | Sound cues, picks its own channels | 7-14 | Stream 4 |
| Reaper | Virtual soundcheck playback, picks its own channels | 15-46 | Streams 5-8 |
| Everything else | Notifications and system sounds | - | Not sent |

### 5. The Mac as the network clock (exploring)

When no PTP grandmaster is present, the Mac provides the clock itself. That allows audio between computers with no console or other hardware in between: Mac to Mac, or Mac to Linux (PipeWire already supports AES67) and Windows.

- Two-computer streaming setups, for example game and voice audio sent as separate streams to a streaming machine.

## Alongside the steps

- **Compatibility list.** A public table of tested devices, firmware versions and results, filled in from testers' reports.
- **Network input hardening.** The driver reads stream announcements, RTP and PTP packets from any device on the network inside the Mac's audio system. The SDP, RTP and PTP parsers refuse malformed fields, PTP state has fixed limits so a flood of Announce or Sync messages can't grow memory, and one malformed RTP packet can't cause the next good one to be dropped. CI replays a fuzz corpus on every change and fuzzes each parser nightly.
- **Failures inside coreaudiod.** A C++ exception in a Core Audio callback, such as a failed allocation, now returns an error instead of terminating coreaudiod, and a start that fails partway is rolled back ([#147](https://github.com/soundsofthesir/AES67_macos_Driver/issues/147)).
- **Smaller fixes.** DSCP marking for PTP and RTP ([#48](https://github.com/soundsofthesir/AES67_macos_Driver/issues/48)), and checking a stream's clock against the grandmaster ([#49](https://github.com/soundsofthesir/AES67_macos_Driver/issues/49)).
- **Resampling (optional).** For senders that run on a different clock and can't follow PTP ([#43](https://github.com/soundsofthesir/AES67_macos_Driver/issues/43)).

## Not planned for now

- Formal SMPTE ST 2110-30 conformance.
- NMOS discovery and control.
- Redundant networks (ST 2022-7).

These matter for broadcast facilities, which usually buy certified products. They may come later if people need them.

## Who it's for

- **Console owners:** desks with a Dante or AES67 card, recording multitrack to a Mac without a paid virtual soundcard.
- **Live streamers:** getting console channels into OBS and sending a stream mix, a call and playback back to the desk as separate channels.
- **Venues and churches:** small teams with networked audio gear and a Mac, but no budget for licensed software.
- **Computer-to-computer setups:** later, after step 5. Moving many channels between machines with no extra hardware.

## How to help

Testing with real hardware helps most. Receiving from a device into the Mac is the most complete path today.

1. Install the latest [pre-release](https://github.com/soundsofthesir/AES67_macos_Driver/releases), or build current `main`. It has mostly been tested on Apple Silicon, so Intel results are useful too.
2. Open an issue with your device and firmware version, your macOS version, the stream's SDP (the `a=`, `c=` and `m=` lines are enough), and what you heard.
3. Include the driver's log from around the time of the problem:

   ```bash
   sudo log show --info --last 10m --predicate 'subsystem == "com.aes67driver"'
   ```

Pull requests are welcome. Small ones against current `main` are easiest to merge.
