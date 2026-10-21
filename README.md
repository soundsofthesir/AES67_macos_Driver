# AES67 macOS Audio Driver

> **Experimental.** Not ready for production use.

An open-source AES67 network audio driver for macOS. It runs as a user-space Core Audio plug-in (AudioServerPlugIn, built on libASPL), so no kernel extension or security changes are needed.

## Status

**Works today (tested in Core Audio with test streams):**

- Receives AES67 streams at a fixed 8 ms latency. Streams from senders on the same clock stay sample-aligned, and several apps can record at once.
- Locks its clock to the incoming stream, so a sender running fast or slow doesn't cause dropouts.
- Rides out short network stalls without losing sync.

**Built and tested, not yet tried in Core Audio:**

- Sending AES67 streams, continuously and on the device clock.
- Announcing sent streams over SAP, so tools like Dante Controller can see them.
- Rejoining streams after a network change (new IP address, Wi-Fi roam, cable replug).

**Experimental, off by default:**

- PTP. With `ptp.json` turning it on (see [Configuration](#configuration)), the driver follows a PTP grandmaster: the device clock runs on PTP time, received audio is placed by it, and sent streams are stamped with it and name the grandmaster in their SAP announcements. Only tested on loopback against test masters so far.

**Not done yet:**

- PTP on a real network. Until that's been tested, Dante and RAVENNA devices can see the driver's streams but probably won't play them.
- Long tests with real hardware. An earlier version received audio from a hardware AES67 device; the rebuilt version hasn't been tried with one yet.

Progress is tracked in the [step 2 plan](Docs/Step2-Clocking-Plan.md).

## Roadmap

Next up is trying PTP on a real network with Dante, RAVENNA and other AES67 gear, then a signed installer, a Manager app that controls the driver, and per-app routing. See the [roadmap](Docs/Roadmap.md) for the full order and how to help.

## Known limitations

- PTP is off by default and untested on a real network, so devices that align to PTP time (Dante, RAVENNA) may not play transmitted streams yet.
- Without PTP the driver follows one sender's clock, and a second sender on a different clock drifts and occasionally resyncs. With PTP the same goes for any sender that isn't on the grandmaster ([#43](https://github.com/soundsofthesir/AES67_macos_Driver/issues/43)).
- The Manager app doesn't control the driver yet.
- The driver can't save settings from inside Core Audio ([#42](https://github.com/soundsofthesir/AES67_macos_Driver/issues/42)); edit `streams.json` instead (see below).
- Dante only subscribes to multicast in its configured range (default 239.69.x.x). The driver warns about streams outside it.
- If you set a stream's interface as an IP address, it breaks when the address changes. Use the interface name (e.g. `en0`).

## Building

```bash
brew install cmake
brew tap gavv/gavv && brew install libaspl

git clone https://github.com/soundsofthesir/AES67_macos_Driver.git
cd AES67_macos_Driver
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
ctest --output-on-failure
```

Install:

```bash
sudo cp -R AES67Driver.driver /Library/Audio/Plug-Ins/HAL/
sudo mkdir -p "/Library/Application Support/AES67Driver"
sudo chown _coreaudiod:wheel "/Library/Application Support/AES67Driver"
sudo launchctl kickstart -k system/com.apple.audio.coreaudiod
```

The driver runs as `_coreaudiod` and needs to own its config directory to save stream changes (the installer does this).

The device appears as "AES67 Device" in Audio MIDI Setup, with 128 inputs and 128 outputs.

## Configuration

Streams are read from `streams.json`, looked for in this order:

1. `$AES67_CONFIG_PATH` (if set, only this path is used, even before the file exists)
2. `~/Library/Application Support/AES67Driver/` (tests and tools only: the installed driver's home is `/var/empty`)
3. `/Library/Application Support/AES67Driver/`

With no config file, the driver creates one test receive stream (239.1.1.1:5004, 8 channels) and no send streams. A config file with no streams, or none that load, stays as it is. Each stream can name a network interface (`en0`) or leave it blank to use the main one.

Streams marked `"enabled": false`, and entries the driver can't read, are kept in the file when it saves. A file that isn't valid JSON is copied to `streams.json.bak` and left in place.

PTP is off unless `ptp.json` turns it on. It's looked for in `$AES67_PTP_CONFIG_PATH`, then the same two Application Support folders:

```json
{ "enabled": true, "interface": "en0", "domain": 0, "hybrid": false }
```

`interface` is a name, an address, or `""` for the main one. `hybrid` sends Delay_Req unicast to the master instead of multicast. With PTP on, the driver sends Delay_Req packets onto the network. The file is read when Core Audio loads the driver, so restart coreaudiod after changing it.

## Testing tools

- `ctest` runs 24 test suites. Tests that send audio or PTP packets stay on your machine and never reach the network.
- `AES67TestSender` sends a test tone **onto your network** (default 239.1.1.1:5004).
- `AES67ClockProbeClient` measures the device's clock and checks the audio is continuous. Build it with `-DBUILD_SPIKES=ON`.

## Project layout

```
Driver/          Core Audio device and IO
NetworkEngine/   RTP send/receive, media clock, SAP, stream management
Shared/          Common types
Tools/           Test sender, receiver and probes
Tests/           Test suites
Docs/            Design plan and investigation write-ups
Installer/       Installer package scripts (unsigned)
Examples/        Small examples of the channel mapper and SDP parser
ManagerApp/      SwiftUI app (not yet connected)
```

## Contributing

Hardware test reports are the most useful thing right now, especially with Dante, RAVENNA or other AES67 gear. Bug reports with steps to reproduce are welcome too. Please write tests for new code and keep the audio thread lock-free.

## License

GPL-3.0. Uses [libASPL](https://github.com/gavv/libASPL) (MIT).

## Thanks

- [libASPL](https://github.com/gavv/libASPL)
- [marcnnn's fork](https://github.com/marcnnn/AES67_macos_Driver), whose testing with a Behringer WING led to the SAP support and several fixes
