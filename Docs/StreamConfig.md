# streams.json and ptp.json

`aes67ctl` writes both files (see the README). This page is for reading them, or for editing them by hand.

The driver reads both files only when Core Audio loads it. After editing either one, restart coreaudiod with `sudo killall coreaudiod` or `sudo aes67ctl restart`.

## Where the driver looks

`streams.json`:

1. `$AES67_CONFIG_PATH`. If it is set, the driver uses only this path, even before the file exists.
2. `~/Library/Application Support/AES67Driver/`. This is for tests and tools only, because the installed driver's home is `/var/empty`.
3. `/Library/Application Support/AES67Driver/`

`ptp.json` is looked for in `$AES67_PTP_CONFIG_PATH`, then in the same two folders.

`aes67ctl` uses the system folder (3), or the two environment variables if they are set.

## streams.json

With no file, the driver creates one test receive stream (239.1.1.1:5004, 8 channels) and no send streams. A file with no streams, or none that load, stays as it is.

The example below has one stream of each kind. The receive stream puts a 1-channel stream on device input 1. The send stream carries device outputs 1 and 2, which are the Mac's stereo output.

```json
{
  "version": "1.0",
  "streams": [
    {
      "enabled": true,
      "description": "",
      "networkInterface": "en0",
      "sdp": {
        "sessionName": "Intercom feed",
        "connectionAddress": "239.69.1.1",
        "port": 5004,
        "ttl": 32,
        "payloadType": 97,
        "encoding": "L24",
        "sampleRate": 48000,
        "numChannels": 1,
        "ptime": 1,
        "framecount": 48,
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
    },
    {
      "enabled": true,
      "description": "",
      "networkInterface": "en0",
      "sdp": {
        "sessionName": "Studio Mac out",
        "sessionID": 2849120193,
        "sessionVersion": 1,
        "connectionAddress": "239.69.1.2",
        "port": 5004,
        "ttl": 32,
        "payloadType": 97,
        "encoding": "L24",
        "sampleRate": 48000,
        "numChannels": 2,
        "ptime": 1,
        "framecount": 48,
        "direction": "sendonly"
      },
      "mapping": {
        "streamID": "0f7d4a52-6c55-4f61-9d0e-0d6a4b0f2a77",
        "streamName": "Studio Mac out",
        "streamChannelCount": 2,
        "streamChannelOffset": 0,
        "deviceChannelStart": 0,
        "deviceChannelCount": 2
      }
    }
  ]
}
```

| Field | Meaning |
|---|---|
| `enabled` | `false` keeps the stream in the file without running it. It still holds its device channels. |
| `networkInterface` | The interface name, such as `en0`. Leave it empty to use the main interface. An IP address also works, but stops working when the address changes. |
| `sdp.connectionAddress`, `sdp.port` | The stream's multicast group and port. |
| `sdp.direction` | `recvonly` receives the stream; `sendonly` sends it. |
| `sdp.numChannels` | 1 to 8 channels. |
| `sdp.encoding` | `L24` or `L16`. |
| `sdp.ptime`, `sdp.framecount` | Packet time in ms, and the samples per packet (48 at 48 kHz and 1 ms). |
| `sdp.ttl` | Multicast TTL for a send stream. 0 keeps it on this Mac. |
| `sdp.sessionID` | A send stream's SAP session ID. Keep it the same across edits, so receivers list the stream only once. |
| `sdp.sourceAddress` | Optional. A send stream announces its interface's current address whatever this says. |
| `mapping.streamID` | A UUID that is unique per stream (`uuidgen` makes one). |
| `mapping.deviceChannelStart` | The first device channel, counted from 0. Inputs and outputs are numbered separately, so a receive stream and a send stream can both start at 0. |

The Mac's stereo output plays on device outputs 1 and 2 (`deviceChannelStart` 0, 2 channels). A 1-channel send stream on output 1 carries only the left channel.

When the driver saves the file, it keeps disabled streams and entries it can't read. If the file isn't valid JSON, the driver copies it to `streams.json.bak` and leaves it in place. `aes67ctl` refuses to change a file that isn't valid JSON.

## ptp.json

PTP is off unless `ptp.json` turns it on:

```json
{ "enabled": true, "interface": "en0", "domain": 0, "hybrid": false }
```

- `interface` is a name, an address, or `""` for the main interface.
- `domain` is 0 to 127.
- `hybrid` sends Delay_Req unicast to the master instead of multicast.

A file that can't be read leaves PTP off. With PTP on, the driver sends about one Delay_Req packet a second onto the network.
