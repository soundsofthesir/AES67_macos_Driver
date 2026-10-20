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

**Not done yet:**

- PTP. Until it lands, Dante and RAVENNA devices can see the driver's streams but won't play them.
- Long tests with real hardware. An earlier version received audio from a hardware AES67 device; the rebuilt version hasn't been tried with one yet.

Progress is tracked in the [step 2 plan](Docs/Step2-Clocking-Plan.md).

## Roadmap

Next up is PTP, then a signed installer, a Manager app that controls the driver, and per-app routing. See the [roadmap](Docs/Roadmap.md) for the full order and how to help.

## Known limitations

- No PTP yet, so devices that align to PTP time (Dante, RAVENNA) won't play transmitted streams.
- The driver follows one sender's clock. A second sender on a different clock will drift and occasionally resync until PTP lands.
- The Manager app doesn't control the driver yet.
- The driver can't save settings from inside Core Audio; edit `streams.json` instead (see below).
- Dante only subscribes to multicast in its configured range (default 239.69.x.x). The driver warns about streams outside it.
- If you set a stream's interface as an IP address, it breaks when the address changes. Use the interface name (e.g. `en0`).

## Building

```bash
brew install cmake
brew tap gavv/gavv && brew install libaspl

git clone https://github.com/maxajbarlow/AES67_macos_Driver.git
cd AES67_macos_Driver
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
ctest --output-on-failure
```

Install:

```bash
sudo cp -R AES67Driver.driver /Library/Audio/Plug-Ins/HAL/
sudo launchctl kickstart -k system/com.apple.audio.coreaudiod
```

The device appears as "AES67 Device" in Audio MIDI Setup, with 128 inputs and 128 outputs.

Apps that pick a device by UID need `com.aes67.driver.device`. JACK is one: `-d` takes the UID, and with anything else (including the name "AES67 Device") it quietly falls back to the system default devices.

```bash
jackd -d coreaudio -d com.aes67.driver.device -r 48000 -p 256
```

## Configuration

Streams are read from `streams.json`, looked for in this order:

1. `$AES67_CONFIG_PATH`
2. `~/Library/Application Support/AES67Driver/`
3. `/Library/Application Support/AES67Driver/`

With no config, the driver creates one test receive stream (239.1.1.1:5004, 8 channels) and no send streams. Each stream can name a network interface (`en0`) or leave it blank to use the main one.

## Testing tools

- `ctest` runs 18 test suites. Tests that send audio stay on your machine and never reach the network.
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
ManagerApp/      SwiftUI app (not yet connected)
```

## Contributing

Hardware test reports are the most useful thing right now, especially with Dante, RAVENNA or other AES67 gear. Bug reports with steps to reproduce are welcome too. Please write tests for new code and keep the audio thread lock-free.

## License

GPL-3.0. Uses [libASPL](https://github.com/gavv/libASPL) (MIT).

## Thanks

- [libASPL](https://github.com/gavv/libASPL)
- [marcnnn's fork](https://github.com/marcnnn/AES67_macos_Driver), whose testing with a Behringer WING led to the SAP support and several fixes
