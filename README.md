# AES67 macOS Audio Driver

> **Experimental.** Not ready for production use.

An open-source AES67 network audio driver for macOS. It runs as a user-space Core Audio plug-in (AudioServerPlugIn, built on libASPL), so no kernel extension or security changes are needed.

## Status

**Tested with real hardware (Riedel Artist intercom, 18 November 2026):**

- Receiving a 1-channel L24 stream from Riedel gear, with and without PTP. The stream had no packet loss over 10 minutes, and the audio played in Core Audio with no dropouts.
- Sending a 1-channel L24 stream, 48 kHz with 1 ms packets, that Artist plays. The packets went out on the interface named in the config, not the Mac's default route.
- PTP on a real network. The driver locked to the grandmaster in about 10 seconds with unicast Delay_Req (`"hybrid": true`), and every Delay_Req got a reply. The measured path delay was 141 µs on one lock and 62 µs on the next.

These were short runs, about 15 minutes, on a dedicated USB Ethernet adapter with a static address.

**Works today (tested in Core Audio with test streams):**

- Receives AES67 streams at a fixed 8 ms latency. Streams from senders on the same clock stay sample-aligned, and several apps can record at once.
- Locks its clock to the incoming stream, so a sender running fast or slow doesn't cause dropouts.
- Rides out short network stalls without losing sync.
- Sends AES67 streams continuously, on the device clock or PTP time, and announces them over SAP.

**Built and tested, not yet tried in Core Audio:**

- Rejoining streams after a network change (new IP address, Wi-Fi roam, cable replug).
- Returning an error instead of terminating coreaudiod when a Core Audio callback hits a C++ exception, such as a failed allocation. A start that fails partway is rolled back.

PTP is off by default. With `ptp.json` turning it on (see [Configuration](#configuration)), the device clock runs on PTP time, received audio is placed by it, and sent streams are stamped with it and name the grandmaster in their SAP announcements.

**Not done yet:**

- Sending to Dante or RAVENNA receivers.
- Stereo and 8-channel streams on hardware.
- Runs of several hours, grandmaster changes, and stopping and restarting a send stream while a receiver is playing it.

Progress is tracked in the [step 2 plan](Docs/Step2-Clocking-Plan.md).

## Roadmap

Next up is more hardware testing (Dante, RAVENNA, longer runs, more channels), then a signed installer, a Manager app that controls the driver, and per-app routing. See the [roadmap](Docs/Roadmap.md) for the full order and how to help.

## Known limitations

- Don't bridge the AES67 interface, for example with Internet Sharing. While the interface is in a bridge, macOS doesn't answer the switch's IGMP queries, so a snooping switch stops forwarding PTP and audio groups about 4 minutes after the driver joins them. PTP lock is lost and receivers drop the Mac's audio ([#197](https://github.com/soundsofthesir/AES67_macos_Driver/issues/197)).
- Sent packets aren't DSCP-marked yet ([#48](https://github.com/soundsofthesir/AES67_macos_Driver/issues/48)).
- Without PTP the driver follows one sender's clock, and a second sender on a different clock drifts and occasionally resyncs. With PTP the same goes for any sender that isn't on the grandmaster ([#43](https://github.com/soundsofthesir/AES67_macos_Driver/issues/43)).
- Received packets that arrive more than 8 ms late play as silence. So do packets that arrive on time but that the receive thread writes after Core Audio has read past them, which happens when the thread is held off the CPU. The driver counts these per stream but doesn't report the count anywhere yet.
- The Manager app doesn't control the driver yet.
- Dante's default AES67 multicast range is 239.69.x.x. The driver logs a warning for send streams outside it.
- If you set a stream's interface as an IP address, it breaks when the address changes. Use the interface name (e.g. `en0`).
- Some apps can't open a 128-channel device. ffmpeg's AVFoundation input fails on it, because its resampler stops at 64 channels.

## Building

```bash
brew install cmake

# libASPL isn't in Homebrew: build it universal and install into /usr/local
git clone --depth 1 --branch v3.1.2 https://github.com/gavv/libASPL.git
cmake -S libASPL -B libASPL/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build libASPL/build -j
sudo cmake --install libASPL/build

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

With no config file, the driver creates one test receive stream (239.1.1.1:5004, 8 channels) and no send streams. A config file with no streams, or none that load, stays as it is. Each stream can name a network interface (`en0`) or leave it blank to use the main one. Both files are read when Core Audio loads the driver, so restart coreaudiod after editing them.

This receive stream was tested with Riedel gear. It puts a 1-channel stream on device input 1:

```json
{
  "version": "1.0",
  "streams": [
    {
      "enabled": true,
      "description": "Intercom feed",
      "networkInterface": "en10",
      "sdp": {
        "sessionName": "Intercom feed",
        "connectionAddress": "239.69.1.1",
        "port": 5004,
        "ttl": 32,
        "payloadType": 96,
        "encoding": "L24",
        "sampleRate": 48000,
        "numChannels": 1,
        "ptime": 1,
        "framecount": 48,
        "sourceAddress": "192.168.1.20",
        "ptpDomain": 0,
        "mediaClockType": "direct=0",
        "direction": "recvonly"
      },
      "mapping": {
        "streamID": "609097ea-20a4-4493-b256-ec048978c223",
        "streamName": "Intercom feed",
        "streamChannelCount": 1,
        "streamChannelOffset": 0,
        "deviceChannelStart": 0,
        "deviceChannelCount": 1
      }
    }
  ]
}
```

For a send stream, set `direction` to `sendonly`, give it its own group and UUID, and set `sourceAddress` to the Mac's address. `streamID` must be a UUID (`uuidgen` makes one). `deviceChannelStart` counts from 0, so 0 is device channel 1. Inputs and outputs are counted separately, so a receive stream and a send stream can both start at 0. The Mac's stereo output plays on device outputs 1 and 2, so a 1-channel send stream on output 1 carries only the left channel.

Receive streams join their multicast group only while an app is using the device. A switch with IGMP snooping won't forward the stream until then. Send streams start when the driver loads and send silence when nothing is playing.

Streams marked `"enabled": false`, and entries the driver can't read, are kept in the file when it saves. They also keep their device channels, so a new stream never lands on a disabled stream's channels and the stream comes back where it was when enabled again. If a running stream and a saved one list the same channels, the running one has them. A file that isn't valid JSON is copied to `streams.json.bak` and left in place.

PTP is off unless `ptp.json` turns it on. It's looked for in `$AES67_PTP_CONFIG_PATH` (if set, only there), then `ptp.json` in the same two Application Support folders:

```json
{ "enabled": true, "interface": "en0", "domain": 0, "hybrid": false }
```

`interface` is a name, an address, or `""` for the main one. `domain` is 0 to 127. `hybrid` sends Delay_Req unicast to the master instead of multicast. A file that can't be read leaves PTP off. With PTP on, the driver sends Delay_Req packets onto the network, about one a second. Check `PTP: locked to grandmaster` in the driver log. If it later logs `PTP: not locked (no master)` every few minutes, the switch has stopped forwarding PTP to the Mac (see [Known limitations](#known-limitations)).

## Testing tools

- CI (`.github/workflows/ci.yml`) builds and runs `ctest` on macOS in Release and with AddressSanitizer + UndefinedBehaviorSanitizer (`-DAES67_SANITIZE=ON`), and runs the tests that need no Apple frameworks on Linux with both sanitizers (`Tests/Linux/run-portable-tests.sh`).
- CI sets `AES67_TEST_RELAXED_TIMING=1`, which widens the tests' limits on timing accuracy (PTP phase, packet departure times, and how many packets a sender gets out in a set time) for shared runners. Ordering and continuity checks don't change. Receive tests excuse only packets their own sender sent late, or that the driver counted as written after playout.
- `Tests/Fuzz/run-fuzzers.sh` fuzzes the parsers that read network packets and config files (PTP, SAP, SDP, `streams.json`, `ptp.json`) with libFuzzer on Linux. CI replays `Tests/Fuzz/corpus` on every PR, and fuzzes each parser for 5 minutes nightly (`.github/workflows/fuzz.yml`).
- `ctest` runs 26 test suites and a check of the driver bundle's signature. Tests that send audio, SAP or PTP packets stay on your machine and never reach the network. Multicast tests fail if the 224.0.0.0/4 route goes through a VPN interface (check `netstat -rn -f inet | grep 224`), or if the terminal app running them lacks Local Network permission (System Settings > Privacy & Security > Local Network).
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
