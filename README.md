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
- Announcing sent streams over SAP.
- Rejoining streams after a network change (new IP address, Wi-Fi roam, cable replug).

**Built and tested on loopback only, off by default:**

- PTP. With `ptp.json` turning it on (see [Configuration](#configuration)), the driver follows a PTP grandmaster. The device clock runs on PTP time, received audio is placed by it, and sent streams are stamped with it and name the grandmaster in their SAP announcements. Tested against scripted masters on the same Mac, not yet on a real network ([#21](https://github.com/soundsofthesir/AES67_macos_Driver/issues/21)).

**Not done yet:**

- Sending to Dante, RAVENNA or other AES67 receivers. The driver's sent streams haven't been played by any of them yet.
- Long tests with real hardware. An earlier version received audio from a hardware AES67 device; the rebuilt version hasn't been tried with one yet.

Progress is tracked in the [step 2 plan](Docs/Step2-Clocking-Plan.md).

## Roadmap

Next up is trying PTP and sending on a real network with Dante, RAVENNA and other AES67 gear, then a signed installer, a Manager app that controls the driver, and per-app routing. See the [roadmap](Docs/Roadmap.md) for the full order and how to help.

## Known limitations

- PTP is off by default and untested on a real network.
- Without PTP the driver follows one sender's clock, and a second sender on a different clock drifts and occasionally resyncs. With PTP the same goes for any sender that isn't on the grandmaster ([#43](https://github.com/soundsofthesir/AES67_macos_Driver/issues/43)).
- Received packets that arrive more than 8 ms late play as silence.
- The Manager app doesn't control the driver yet.
- Dante's default AES67 multicast range is 239.69.x.x. The driver logs a warning for send streams outside it.
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
sudo killall coreaudiod
```

The driver runs as `_coreaudiod` and needs to own its config directory to save stream changes (the installer does this). With SIP on, `launchctl kickstart` can't restart coreaudiod; `killall` can. Restart it as rarely as you can: on macOS 26.0.1, about 20 restarts in one session left system audio daemons using all CPU until logout.

The device appears as "AES67 Device" in Audio MIDI Setup, with 128 inputs and 128 outputs.

Apps that pick a device by UID need `com.aes67.driver.device`. JACK is one: `-d` takes the UID, and with anything else (including the name "AES67 Device") it quietly falls back to the system default devices.

```bash
jackd -d coreaudio -d com.aes67.driver.device -r 48000 -p 256
```

## Configuration

Streams are read from `streams.json`, looked for in this order:

1. `$AES67_CONFIG_PATH` (if set, only this path is used, even before the file exists)
2. `~/Library/Application Support/AES67Driver/` (tests and tools only: the installed driver's home is `/var/empty`)
3. `/Library/Application Support/AES67Driver/`

With no config file, the driver creates one test receive stream (239.1.1.1:5004, 8 channels) and no send streams. A config file with no streams, or none that load, stays as it is. Each stream can name a network interface (`en0`) or leave it blank to use the main one.

Streams marked `"enabled": false`, and entries the driver can't read, are kept in the file when it saves. They also keep their device channels, so a new stream never lands on a disabled stream's channels and the stream comes back where it was when enabled again. If a running stream and a saved one list the same channels, the running one has them. A file that isn't valid JSON is copied to `streams.json.bak` and left in place.

PTP is off unless `ptp.json` turns it on. It's looked for in `$AES67_PTP_CONFIG_PATH` (if set, only there), then `ptp.json` in the same two Application Support folders:

```json
{ "enabled": true, "interface": "en0", "domain": 0, "hybrid": false }
```

`interface` is a name, an address, or `""` for the main one. `domain` is 0 to 127. `hybrid` sends Delay_Req unicast to the master instead of multicast. A file that can't be read leaves PTP off. With PTP on, the driver sends Delay_Req packets onto the network. The file is read when Core Audio loads the driver, so restart coreaudiod after changing it.

## Testing tools

- `ctest` runs 25 test suites and a check of the driver bundle's signature. Tests that send audio, SAP or PTP packets stay on your machine and never reach the network. Multicast tests fail if the 224.0.0.0/4 route goes through a VPN interface (check `netstat -rn -f inet | grep 224`), or if the terminal app running them lacks Local Network permission (System Settings > Privacy & Security > Local Network).
- `AES67TestSender` sends a test tone **onto your network** (default 239.1.1.1:5004).
- `AES67ClockProbeClient` measures the device's clock and checks the audio is continuous. Build it with `-DBUILD_SPIKES=ON`.
- Driver logs go to the unified log: `sudo log show --info --predicate 'subsystem == "com.aes67driver"'`, or `log stream` with the same predicate to watch live.

## Project layout

```
Driver/          Core Audio device and IO
NetworkEngine/   RTP send/receive, media clock, SAP, stream management
Shared/          Common types
Tools/           Test sender, receiver and probes
Tests/           Test suites
Docs/            Design plan and investigation write-ups
Installer/       Installer package scripts (unsigned)
Examples/        Channel mapper and SDP parser examples
ManagerApp/      SwiftUI app (not yet connected)
```

## Contributing

Hardware test reports are the most useful thing right now, especially with Dante, RAVENNA or other AES67 gear. Bug reports with steps to reproduce are welcome too. Please write tests for new code and keep the audio thread lock-free.

## License

GPL-3.0. Uses [libASPL](https://github.com/gavv/libASPL) (MIT).

## Thanks

- [libASPL](https://github.com/gavv/libASPL)
- [marcnnn's fork](https://github.com/marcnnn/AES67_macos_Driver), whose testing with a Behringer WING led to the SAP support and several fixes
