# Step 2: Clocking Redesign Plan

| Clock | Drives | Source today |
|---|---|---|
| Core Audio device clock | When Core Audio reads and writes the device | libASPL default `GetZeroTimeStampImpl`: the Mac's host clock at the nominal rate |
| RX consume thread | When received packets move into the ring buffers | `steady_clock` timer, now steered to the sender's packet rate (step 1) |
| TX thread | When packets are sent | `steady_clock` timer at a fixed 1 ms |
| Remote sender | When packets arrive | Its PTP-locked media clock |

Consequences: sender-versus-Mac drift accumulates in the ring buffers until they under- or overflow; received RTP timestamps are ignored, so latency is not fixed and streams are not sample-aligned with each other; transmitted RTP timestamps start at 0, so PTP-aligned receivers (Dante in AES67 mode, RAVENNA) cannot play them; two apps recording at once drain each other's input.

## Goal

One media clock for the whole device. Core Audio's sample clock, RX placement and TX timestamps all derive from it. When PTP is available the media clock is PTP; when it is not, the device follows a received stream or, last resort, the host clock.

### Success criteria

1. Simulated 8-hour run with a sender at +/-100 ppm: zero underruns or overruns, latency constant to +/-1 sample.
2. Two streams from senders on the same PTP grandmaster arrive sample-aligned in Core Audio.
3. A 1-hour RX capture from real hardware (Riedel Artist) shows no discontinuities in a test tone.
4. A Dante or RAVENNA receiver plays the driver's TX stream without dropouts.
5. Two apps recording simultaneously receive identical input.

## Verified facts this plan rests on

- **Kernel receive timestamps are already in Core Audio's time base.** `SO_TIMESTAMP_MONOTONIC` returns `mach_absolute_time` ticks (verified on this Mac: each timestamp fell between `mach_absolute_time` reads taken before the send and after the receive, 10-20 us ahead of userspace). PTP event timestamps can therefore be used directly against Core Audio host time, with no clock translation.
- **Host ticks are not nanoseconds.** `mach_timebase_info` is 125/3 on Apple Silicon. Nothing in the repo uses it today.
- **Core Audio follows a model-driven device clock exactly** (spike S2).
- **Zero timestamps map sample time to `mach_absolute_time`** (AudioServerPlugIn.h). The period must be at least 10923 frames. Changing the seed tells the HAL the timeline restarted. `kAudioDeviceClockAlgorithmRaw` disables HAL filtering, appropriate when timestamps come from a servo model.
- **Nothing on this Mac currently owns UDP 319/320, and the sandboxed driver host can use them** (spike S1).
- **The existing PTP, PLL and resampler code is not a foundation** (separate assessment): `PTPSlave` deadlocks on every Follow_Up and Sync path, compares TAI to UTC so it can never lock, has no frequency estimate and no host-time mapping; `PhaseLockedLoop` is open-loop; the resampler resets phase every block. Only `PTPSlave`'s socket setup, packet field parsing and Delay_Req builder are worth carrying forward.

## Architecture

```
                        +-----------------------------+
   PTP (319/320) -----> |  ClockSource (one active)    |
   RX RTP timestamps -> |   PTP | StreamRecovered | Host|
                        +--------------+--------------+
                                       | publishes model (seqlock)
                                       v
                        +-----------------------------+
                        |  MediaClock                  |
                        |  host ticks <-> media sample |
                        +---+-------------+-----------+
                            |             |
        GetZeroTimeStampImpl|             | toHost(M), toMedia(h)
                            v             v
  Core Audio IO --> device sample T --> media sample M = T + mediaBase
        |                                          ^
        | read [M - linkOffset, +n)                | write at RTP-derived M
        v                                          |
  +-----------------------------+      +---------------------------+
  | PlayoutBuffer (per RX stream)| <--- | Receive thread (decode)   |
  | slots tagged by media sample |      | no consume thread         |
  +-----------------------------+      +---------------------------+
        | write mix at M                     
        v                                    
  +-----------------------------+      +---------------------------+
  | TxBuffer (per TX stream)    | ---> | TX thread: wakes at        |
  | slots tagged by media sample|      | toHost(M + F), sends RTP   |
  +-----------------------------+      | ts = M + mediaclk offset   |
                                       +---------------------------+
```

### MediaClock

An affine map between host time and media position, published lock-free so the IO thread can read it:

```
media(h) = anchorMedia + (h - anchorHost) * samplesPerTick
host(M)  = anchorHost  + (M - anchorMedia) / samplesPerTick
samplesPerTick = fs * rateRatio * timebase.numer / (timebase.denom * 1e9)
```

- With PTP, media position maps to PTP time in samples (`ptpNs * fs / 1e9`) through a whole-sample offset, `M - ptpSamples`, chosen when PTP takes the clock. Every device on the grandmaster agrees on PTP time, so RX and TX use the offset to convert. (Revised 2026-10-05: an earlier draft made media position equal PTP time. That would jump the device clock when PTP locks, and move it backwards when a grandmaster with an earlier epoch takes over, where stale buffer slots could read as current. With the offset, media positions only move forward and taking over needs no jump.)
- Updated by the active clock source a few times per second; readers extrapolate between updates. Rate changes are smooth (servo-filtered); a phase step bumps the generation, which becomes the HAL seed.
- Publication via a seqlock or double-buffered snapshot; readers never block.

### Device clock

Override `GetZeroTimeStampImpl`:

- At `StartIO`, choose `mediaBase` (a multiple of the period near the current media position), so device sample time `T = M - mediaBase` starts near 0.
- Zero timestamp: `k = floor((media(now) - mediaBase) / P)`, return `sampleTime = k * P`, `hostTime = host(mediaBase + k * P)`, `seed = generation`.
- `GetClockAlgorithm` returns Raw; `GetClockIsStable` reflects lock state; `GetClockDomain` returns a non-zero ID derived from the grandmaster identity and domain, so other devices on the same PTP domain are reported as synchronised.
- Sample rate changes rebase `mediaBase`.

### RX: playout by timestamp

- RTP timestamp to media position: unwrap `(rtpTs - mediaclkOffset) mod 2^32` to the value nearest `media(now)` (the `a=mediaclk:direct=` offset from the SDP, per stream).
- The receive thread decodes each packet straight into a per-stream `PlayoutBuffer` at its media position, tagging the slot with that position. No jitter buffer, consume thread, prefill, rate controller or resync logic.
- The IO handler reads each mapped channel at `M - linkOffset` for the requested range. A slot whose tag does not match the requested position reads as silence. Loss, outage and restart handling become structural: a missing packet's slot is stale, so it plays as silence; a restarted sender's timestamps place it correctly; latency cannot grow.
- Reads are non-destructive, so any number of clients read identical input.
- Counters: late (arrived after playout), early (beyond the buffer horizon, signals clock mismatch), missing.
- Keep the step 1 SSRC gate so a second sender on the group cannot interleave.
- `linkOffset` is configurable and reported to the HAL as input latency.

### TX: timestamps from the media clock

- `OnWriteMixedOutput` writes the mix into each TX stream's `TxBuffer` at `M = T + mediaBase`.
- The TX thread wakes at `host(M + F)` for each packet of F frames (from ptime and the current rate, fixing the 48 kHz-only limitation), reads `[M, M + F)`, and sends RTP with `ts = (M + mediaclkOffset) mod 2^32`. Slots not written (no client running) send silence.
- Packets leave just after their last sample's media time, as hardware senders do, so they are neither early nor late at receivers.
- Generated SDP gains correct `a=ts-refclk` and `a=mediaclk:direct=` lines.

### Clock sources

| Source | When used | Accuracy |
|---|---|---|
| PTP | Locked to a grandmaster on the configured domain | Frequency to well under 1 ppm; phase limited by software timestamping (measure in spike S3; expect tens of microseconds) |
| StreamRecovered | No PTP, at least one RX stream: fit RTP timestamps against arrival host time for a reference stream | Drift-free relative to that sender; arrival jitter averaged out |
| Host | Nothing else available | Free-running, today's behaviour |

Selection is automatic with a configurable preference, and switching sources is a phase step (new seed).

### PTP slave (new)

#### Target network (from the Riedel Artist SIC AES67 card's PTP settings, October 2026)

| Setting | Value | What the slave must do |
|---|---|---|
| PTP mode | Hybrid | Sync, Follow_Up and Announce arrive by multicast; **Delay_Req is sent unicast to the master's address** (the source of its Sync/Announce), and Delay_Resp arrives unicast on port 320. Keep multicast Delay_Req as an option for non-hybrid networks. |
| Role | TimeReceiver (slave) | The Riedel is not the grandmaster; a separate device on the network is. |
| Domain | 0 | The default. |
| Announce interval / receipt timeout | 1 (2 s) / 3 | Derive the timeout from the received `logAnnounceInterval` (here 6 s), never a fixed value. |
| Sync interval / delay request interval | 0 / 0 (1 per second) | These are the Riedel's own port settings; the grandmaster sets the real rates, which spike S3 must measure. If Sync really is 1 Hz, the servo gets one measurement per second and needs a longer averaging window than an 8 Hz network. |
| Media 1 / Media 2 | Shared PTP settings | Suggests dual networks (SMPTE ST 2022-7). The driver uses one network; 2022-7 redundancy is out of scope for step 2. |

From a SmartPanel's AES67 tab (RSP-1232HL):

| Setting | Value | Implication |
|---|---|---|
| Packet time | 1.000 ms | 48 frames at 48 kHz, matching the Riedel SDP in `Docs/Examples/`. |
| Receive buffer | 8.000 ms (8 x packet time) | The network's receive latency convention; basis for the default link offset (decision 1). Also the arrival window our TX packets must meet in phase 5. |
| Play mode | synton | Unconfirmed reading: syntonised playout (frequency-locked, latency set by the buffer) rather than absolute time alignment. If so, TX interop needs the right rate more than PTP phase, which phase 3's recovered clock already provides. Check Riedel's documentation. |
| Media 1 address | 10.46.70.211, port 6060 | Same address range as the development Mac, but during spike S1 that Mac heard no PTP on 224.0.1.129, so S3 needs a port on the PTP network. |

The card also has an NMOS tab, so streams there may be managed through NMOS (IS-04/IS-05) rather than SAP or SDP files. That is a candidate for after step 2.

- Reuse from `PTPSlave`: socket setup (plus `SO_TIMESTAMP_MONOTONIC`), header and Announce field parsing, Delay_Req builder, requesting-port matching.
- Write fresh:
  - Delay_Resp on the general port.
  - Correction field subtracted correctly.
  - Sync and Follow_Up filtered by the selected master's port identity.
  - Full BMCA dataset comparison with foreign-master qualification.
  - Announce timeout from the received interval.
  - No lock held across calls.
- Servo: (host ticks, PTP ns) pairs into a windowed least-squares fit or a PI loop with a frequency integrator, with minimum-delay outlier rejection (software timestamps are positively skewed). It outputs the MediaClock model directly.
- t3 (Delay_Req send time) is taken in userspace just before `sendto`. An early t3 only makes a delay look longer, which the servo's minimum filter discards; a late one would make it look shorter, which the filter would keep. Any remaining bias can be absorbed by a calibration offset.
- Holdover on master loss keeps the last rate; re-acquisition slews if the error is small, otherwise steps (new seed).
- Diagnostics snapshot (state, master, offset, rate, path delay, counters) for the Manager app once the control path exists.

#### Lessons from marcnnn's fork (reviewed October 2026)

marcnnn's `feat/aes67-wing-interop` branch ran a PTP slave against a Behringer WING (Dante) for several days. None of its PTP code is being ported: the slave and the agent have defects listed below. These findings shape the rewrite:

- **Offset: take the least-delayed sample, after detrending.** Software timestamps are only ever late, so the minimum offset over a window tracks the clocks rather than the queueing. Take it after removing the fitted slope; otherwise it lags by drift × window (about 8 µs at 4 ppm over 2 s). This is the same argument as phase 3's `ClockServo`.
- **Frequency: a windowed least-squares slope, not a fast integrator.** Against the WING, a fast integrator swung between 328 and 2444 ppb; a least-squares fit over 256 samples (about 64 s at the WING's 4 Sync/s) held a 1450-1850 ppb band.
- **Time base: host ticks only, never `CLOCK_REALTIME`.** macOS `timed` slews and steps the wall clock. The fork timed everything on it, so an NTP step inside the fit window biases the slope by about 1.5 × step / window (50 ms gives about 1200 ppm).
- **Judge lock independently.** The fork declared lock when the residual against its own correction was under 10 ms, which its P term guarantees, so it reported lock even on mixed masters.
- **Delay_Resp arrives on the general port, 320.** The fork listened for it only on 319, never measured path delay, and blamed the master for not answering. It also added the Delay_Resp correction field to t4 (it must be subtracted) and ignored the correction field on one-step Sync.
- **Filter by the master's port identity.** The fork matched Follow_Up to Sync by sequence ID alone and accepted Sync from any source on the domain, so two masters or a BMCA change interleave t1 and t2 from different clocks. Its BMCA compared only priority1, class and priority2; one stray Announce switched master.
- **Announce timeout from the master's `logMessageInterval`.** It confirms the requirement already in the target-network table.
- **Grandmasters may use an arbitrary epoch.** The WING's PTP time is uptime-based, about 261,432 s from zero. Media position comes straight from PTP time, so nothing needs normalising, but diagnostics should show it, and it is a good test vector. The WING's grandmaster identity is `00-1D-C1-FF-FE-D1-7B-F3`.
- **Run PTP in-process (spike S1), not in a helper.** The fork's LaunchAgent shared time through a world-writable file in `/tmp` that coreaudiod maps. That allows a symlink attack on the user's files, lets any local process crash the driver host (truncate it and the next read faults) or forge grandmaster time, and has two writers when two users are logged in. The reason given for the helper ("the sandbox blocks PTP") was never root-caused, and contradicts S1. If a helper is ever needed, use a root LaunchDaemon with XPC.
- **Frequency range seen in practice:** the WING ran at -42.7 ppm against a Mac mini, well inside the phase 3 servo's +/-2000 ppm.

### ASRC (optional, last)

Only needed for streams not on the device's clock: a different PTP domain, or several unsynchronised senders without PTP. Use an established variable-ratio library rather than the existing linear resampler:
- **libsamplerate** (BSD-2): smooth ratio changes, no allocation after setup.
- **soxr** (LGPL): lower latency.

The repo is GPL-3.0, so GPL options such as zita-resampler are also acceptable. Run the ASRC on the receive thread, never the IO thread, and drive its ratio from the playout buffer's position error.

## What this replaces

The step 1 receive pacing machinery (consume thread, `LockFreeCircularJitterBuffer`, `RateController`, prefill, resync and underrun credit) is superseded by timestamp placement. Step 1 was still necessary to make the current design behave; step 2 makes those behaviours structural. The step 1 regression tests keep their intent (loss plays as silence, restarts recover, outages do not add latency, second senders are ignored) and are rewritten against the new design.

Also removed: `PTPClock`, `PTPDInterface`, `PhaseLockedLoop`, `CustomSys`, `NetworkEngine/Resampling/*`, `Tests/TestPTPClock.cpp`, and the vendored ptpd source (kept "for reference" but never built).

## Phases

Each phase is a separate PR, test-first, and leaves the driver working.

### Phase 0: Foundations and spikes (done)

- `NetworkEngine/Clock/`: `HostTime.h` (mach timebase conversions), `MediaClock.h` (seqlock-published model, whole sample plus fraction for sub-sample precision at PTP-epoch scale), and `TimestampedAudioBuffer` (per-frame tags used as seqlocks). The PlayoutBuffer and TxBuffer in this plan are both instances of `TimestampedAudioBuffer`.
- Covered by `Tests/TestClockFoundations.cpp`. Its stress tests fail against deliberately broken seqlocks (verified by mutation), and the suite is clean under ThreadSanitizer.
- Deterministic unit tests with virtual time:
  - Mapping round-trips.
  - Wraparound.
  - Late and early writes.
  - Concurrent writer with multiple readers.
- Spikes S1-S3 below, before Phase 1 commits to an approach.
- No behaviour change.

### Phase 1: Device clock from MediaClock (done)

- `GetZeroTimeStampImpl` override with the Host source, Raw algorithm, seed handling and rate-change rebase.
- Exit: zero timestamps monotonic and period-aligned in tests; in the real HAL, measured device rate equals nominal and audio is unchanged.
- Implementation:
  - The `mediaBase` idea became a timeline **origin** stored in the `MediaClock` snapshot, set by `reset` and kept by `setRate` and `step`. Device sample time is `T = M - origin`, published atomically with the rest of the clock, so it cannot race. `NetworkEngine/Clock/DeviceTimeline.h` computes zero timestamps as a pure function of a snapshot.
  - The device restarts its timeline (new seed) at IO start and on sample rate changes.
- Result:
  - **Tests:** `TestClockFoundations` covers timestamp alignment, bracketing, exact host mapping and monotonicity across a 10 s sweep. `TestCriticalPathRegressions` checks the device's timestamps land exactly on its own MediaClock, which libASPL's default clock would not.
  - **Real HAL:** with the installed driver, the client (`AES67ClockProbeClient --uid com.aes67.driver.device --constant 0 --no-ramp --expect-level 0.25`) measured the rate at +0.0 ppm (HAL actual rate 48000.000 Hz), with 0 timeline jumps and 0 overloads in 2345 callbacks.
  - **Receive audio:** host-only test packets through a receive-only config arrived unbroken: 100.0% of 1,200,640 input samples were at the expected level.

### Phase 2: RX by timestamp (done; verified in the real HAL)

- Receive thread decodes into PlayoutBuffers; IO handler reads by sample time with link offset; consume thread and jitter buffer deleted; link offset reported as latency.
- Add a tone-continuity analyser tool (extend `QuickCapture`) for long hardware captures.
- Exit: simulated two-stream alignment exact; loss, restart, outage and second-sender tests pass against the new design; two-client reads identical.
- Implementation:
  - `RtpPlacement` maps RTP timestamps (minus the SDP mediaclk offset) to media positions through one device-wide `NetworkTimeMapping`, so streams on the same network timeline stay sample-aligned.
  - `RxRouting` gives the IO thread a lock-free table of receive buffers, with reclamation so a buffer is never freed during a read.
  - Arrival times are kernel timestamps (`SO_TIMESTAMP_MONOTONIC`).
  - The consume thread, `LockFreeCircularJitterBuffer` family, `RateController` and `NetworkEngine/Resampling` were deleted.
  - Driver logging moved to the unified log (`com.aes67driver`), because the sandbox blocks the old `/tmp` file log.
  - The tone-continuity check became `AES67ClockProbeClient --sawtooth`.
- Burst finding:
  - **What the real HAL showed:** a sender stall followed by a catch-up burst re-anchored the timeline twice: late, then early.
  - **Fix:** a re-anchor now needs a run of at least half a link offset whose margins agree to within a quarter of a link offset. A burst's margins grow by a packet each, so it fails that test. Transient stalls drop their late packets but no longer move the timeline.
  - **Second finding (2026-10-03):** the HAL check still re-anchored about twice a minute with the Python test sender. Recording its packets' kernel arrival times showed why. After a 15 ms stall, the sender sent its next 4 packets at normal pace, all 15 ms late, before bursting the rest. Four packets with agreeing margins passed the rule above, so the timeline moved, and the catch-up then moved it back. With a real-time priority sender, the same run had no re-anchors.
  - **Fix:** a timeline change persists; a stall does not. A confirming run must now also cover 50 ms of media (`RtpPlacement::kSourceSwitchSeconds`). A sender restart or SSRC change now plays up to 50 ms more silence before it is followed. `TestRxTimestamp` replays the observed stall (15 ms, 4 steady packets) and a 22 ms, 10-packet stall; both re-anchored twice before the fix.
- Real HAL so far (installed driver, two host-only streams, two client processes):
  - input latency reported as 384 frames (8 ms)
  - +0.0 ppm, 0 timeline jumps, 0 overloads
  - **identical input for two simultaneous clients in all 1876 shared IO cycles**
  - streams sample-aligned except around the burst events above
- Real HAL, 2026-10-03, phase 3 build (which includes this phase), two host-only streams from a sender 200 ppm fast, two clients, real-time priority sender:
  - 0 re-anchors, 0 timeline jumps, 0 overloads
  - test sawtooth continuous: 0 breaks in 2.88 M samples
  - two streams sample-aligned: 0 of 2.88 M frames differ
  - two clients identical in all 1876 shared IO cycles
- Real HAL with the 50 ms rule, 2026-10-03, using the Python sender as a natural stall generator:
  - 120 s run: 0 re-anchors, 0 sawtooth breaks, 0 timeline jumps, two clients identical in all 1878 shared cycles
  - 60 s run with kernel arrival timestamps recorded: the sender stalled beyond the 8 ms link offset 14 times (worst 23.5 ms), still with 0 re-anchors and 0 breaks. Packets later than the link offset play as silence. The two streams differ in a few hundred frames, consistent with a stall falling between the sender's packets for the two streams.
- Pending:
  - **Interface re-resolution: done (2026-10-03).**
    - **The defect:** the interface setting was resolved to an address at config load, and since #14 that address was saved over the setting.
    - **The fix:**
      - Settings are kept as written and resolved at each start.
      - Sockets join and send by interface index (`MCAST_JOIN_GROUP`, `IP_MULTICAST_IFINDEX`).
      - A `NetworkMonitor` (getifaddrs once a second) restarts a changed interface's streams and re-announces its TX streams.

### Phase 3: Stream-recovered clock (implemented; hardware validation pending)

- Device clock follows a reference RX stream's RTP rate.
- First user-visible payoff: drift-free RX from a single sender (the Riedel) without PTP.
- Exit: success criterion 1 in simulation; criterion 3 on hardware.
- Implementation:
  - **Measurement.** Each accepted packet's margin is how far ahead of the read point its end landed: `position + frames - (arrival - link offset)`, using the kernel arrival time and the sub-sample clock position. If the local clock is fast relative to the sender, margins shrink; if slow, they grow.
  - **`ClockServo`** (`NetworkEngine/Clock/ClockServo.h`) is a PI loop on that margin. Jitter only ever delays packets, so it measures the largest margin in each 100 ms window, then takes the median of the last 5 windows: under heavy jitter a window can hold no undelayed packet, and one such window must not kick the rate. It is critically damped at 0.1 rad/s (settles in about 40 s), clamped to +/-2000 ppm, and integrates over the nominal window period so an outage does not weigh one stale error by its length.
  - **`RecoveredClockSource`** chooses the reference: the first stream to report. Other streams are ignored until the reference has been silent for 1 s, when the next one takes over at the current rate. A stopped reference is released with the rate held. When a stream's margins jump (its placement re-anchored, the shared network mapping moved, or the timeline restarted), the servo re-learns its reference margin bumplessly, folding the proportional term into the integral so the rate does not step.
  - **Device.** `AES67Device` owns the source. Rate updates call `MediaClock::setRate` at the current host time, under the clock write lock. A timeline restart (IO start, sample rate change) resets the source first, so no rate steered against the old timeline lands on the new one. Correction (2026-10-03): this originally said TX runs on the same clock and so is frequency-locked to the received stream. It did not: TX was still paced by the host clock and read Core Audio's output first-in-first-out, so with a drifting sender the output rings would slowly fill or drain. Phase 5's media-paced transmitter (below) makes it true, which may be what the Riedel's "synton" play mode needs.
- Result:
  - `Tests/TestClockRecovery.cpp` runs the real `RtpPlacement` against a simulated drifting sender. Without the servo, a 100 ppm sender re-anchors repeatedly. With it, 8 hours at +/-100 ppm gave no re-anchors and no drops, latency constant to +/-1 sample once settled, the rate within 0.5 ppm of the sender's, and no rate step above 5 ppm. A 1000 ppm sender acquired without leaving the window; 3 ms of jitter stayed locked.
  - Over multicast loopback through a real `RTPReceiver`, with a faster test servo (1 rad/s), a sender 1500 ppm fast re-anchored without recovery. With recovery it played with 0 re-anchors and 0 silent samples, and the clock ran at the sender's rate to within about 40 ppm averaged over 3 s. `TestCriticalPathRegressions` checks the real device steers its clock and returns to nominal on an IO restart.
  - Key tests were checked by mutation (no median filter, non-bumpless reacquire, takeover without the silence check, takeover resetting the rate, time-weighted integral), and the suites run clean under ThreadSanitizer.
  - Real HAL (2026-10-03): with two host-only streams from a real-time priority sender 200 ppm fast, Core Audio's measured device rate converged on +200 ppm. It peaked at +228 ppm around 17 s, paying back the phase error built up during acquisition, then decayed (+204 ppm by 55 s). Audio was continuous: 0 re-anchors and 0 breaks in 2.88 M samples. In a first run, every 3 s window from 25 s on was within 8.3 ppm of +200 (most within 3 ppm). Only one coreaudiod restart was used for the whole session.
  - Pending: criterion 3 on the Riedel.
  - Limitation: one reference clock. A second sender on an unrelated clock still drifts against the device and re-anchors; its re-anchors of the shared mapping make the reference re-learn its margin rather than kick the rate. PTP (phase 4) or ASRC (phase 6) handles it.

### Phase 4: PTP

- New slave and servo as the PTP clock source, source selection, clock domain, diagnostics.
- Tests:
  - Golden-vector message parsing, including the WING's grandmaster identity and uptime-based epoch.
  - Servo simulation with jittered, skewed timestamps, including a wall-clock step (which a host-tick servo must not see) and two masters on one domain.
  - Loopback against a scripted PTP master.
  - Hardware run against the Riedel or a Dante device as grandmaster.
- Exit: frequency lock within about 30 s and phase settled within about 2 minutes at a 1 Hz Sync rate (faster on faster networks); hybrid-mode unicast Delay_Req verified against the target network; criterion 2.
- Implemented so far (2026-10-05):
  - Messages, best master, servo and time receiver (#30 to #33), tested in simulation and against scripted masters on loopback.
  - **PTP as the device clock source** (`PtpClockSource`, `PtpClockControl`):
    - The media clock runs at the servo's rate, and phase error is removed by a rate correction of at most 100 ppm over about 2 s.
    - Taking over picks the whole-sample offset nearest the clock as it is, so there is no jump and no new seed.
    - A new PTP timeline (another master, a step) or a phase error over 1 ms picks a new offset instead.
    - Before lock, and while a new master is acquired, the rate is held.
    - While PTP has the clock, the recovered source does not steer it.
    - Clock stability is reported from lock, and the clock domain from the grandmaster identity and domain.
  - **Off by default.** `ptp.json` (next to `streams.json`) turns it on: `{"enabled": true, "interface": "en0", "domain": 0, "hybrid": false}`. Anything malformed leaves it off. The receiver retries every 2 s until its interface exists.
  - **RX and TX on PTP time** (5b):
    - While PTP has the clock, the device fixes the shared network time mapping at PTP's offset. Receivers place each packet by its timestamp (minus the stream's mediaclk offset) at PTP time plus the offset, whenever it arrives.
    - Placement never moves a fixed mapping. A stream off PTP time gets an anchor of its own, but only after a sustained run: one straggler stamped before its sender moved onto PTP earns nothing. A new PTP offset makes every receiver start over against it.
    - Transmitters stamp `M - K + mediaclk`, and carry on unbroken from the last timestamp if PTP lets go.
    - Once a grandmaster is followed, TX streams announce `ts-refclk:ptp=IEEE1588-2008:<grandmaster>:<domain>` (AES67's form, as Dante and RAVENNA write it) and `mediaclk:direct=<offset>`. A new grandmaster re-announces them with a new session version.
    - Result: TX into our own RX through the fixed mapping lands every sample at the position it was sent from. On loopback with a scripted master, the device's TX packets arrive 3 to 22 samples after the master's time reaches their end (send latency only).
  - **Ready for hardware** (2026-10-05):
    - When the PTP interface changes (new address, link down and up, adapter replugged), the receiver starts again on it, as streams do. Its timeline numbers keep rising, so a different master behind the new link is never mistaken for the old one. Meanwhile the clock keeps its rate and is reported unlocked.
    - Transitions are logged (`log stream --info --predicate 'subsystem == "com.aes67driver"'`): receiving, locked to which grandmaster with path delay and rate, not locked, each new offset, interface changes.
    - SDP lines end in CRLF (RFC 4566).
  - Pending: a hardware run (needs sign-off: the slave sends Delay_Req), and criterion 4 with a Dante or RAVENNA receiver.

### Phase 5: TX on the media clock (in progress)

- TxBuffer, media-paced sender, RTP timestamps from media time, ptime and framecount from the rate, compliant SDP.
- Exit: TX to RX loopback sample-exact; criterion 4 with a Dante or RAVENNA receiver.
- Brought forward (2026-10-03) so TX streams can be announced over SAP whenever configured, which needs TX to run continuously. Implemented so far:
  - **TxBuffer.** Each TX stream owns a `TimestampedAudioBuffer`, published through `TxRouting` (the same lock-free table as `RxRouting`). `OnWriteMixedOutput` writes the mix into it at media position `origin + sampleTime`. The 128 SPSC output rings are gone.
  - **Media-paced sender.** The packet holding `[M, M + F)` is sent at `hostAt(M + F)` (`mach_wait_until`), with `F` = sample rate × ptime. Unwritten positions go out as silence. A thread that falls more than 4 packets behind skips to the current packet rather than bursting late packets.
  - **RTP timestamps.** `ts = M + offset`, starting at a random value (RFC 3550). On a timeline restart the offset is adjusted so timestamps and sequence numbers continue without a jump. With PTP (phase 4), the offset becomes the SDP mediaclk offset.
  - **Continuous.** Transmitters run whenever their stream is configured, not just while Core Audio IO runs. The built-in TX test stream is no longer created, so nothing transmits unasked.
  - **Media positions are never reused.** A timeline restart starts 2^20 frames beyond the old position, so no buffer slot from an earlier timeline can read as current. Device sample time still restarts at 0.
- Result:
  - `Tests/TestTxMediaClock.cpp` writes samples whose values are their own media positions:
    - every packet holds the next F positions, at a fixed offset from its RTP timestamp
    - packets leave a median of about 0.2 ms after their last sample's media time (p99 under 0.3 ms), never early
    - with the clock at +1000 ppm, the packet rate measured +998 to +1000 ppm
    - silence flows when nothing is written
    - a timeline restart leaves RTP continuous
    - TX into our own receiver on one clock was sample-exact: 72,000 samples with 0 discontinuities
  - Clean under ThreadSanitizer.
- SAP announcements (2026-10-03), the reason TX was brought forward:
  - **`SAPAnnouncer`** (RFC 2974) announces each TX stream at once, re-announces it every 30 s randomised by +/-1/3, and sends a deletion (same message ID hash) on removal or shutdown.
  - **Announced SDP.** `StreamManager` announces from the stream's own interface and TTL, with `recvonly`, the interface address in `o=`, framecount from the rate, and a random session ID saved with the stream.
  - **Clock reference.** Until PTP, it signals the Mac's own clock with `ts-refclk:localmac=<interface MAC>` (RFC 7273) rather than claiming a PTP reference.
  - **Dante prefix.** TX groups outside Dante's default AES67 prefix (239.69.0.0/16) are logged: marcnnn found with a WING that Dante lists such streams but never subscribes.
  - **Tests.** Live tests announce on a test port with TTL 0. A capture on en0 during the full suite sees no test traffic leave the Mac. Earlier TX tests used TTL 32 and did reach the LAN; that is fixed.
- Lessons from marcnnn's fork, which transmitted to a WING (Dante):
  - **Background timer coalescing.**
    - **Symptom:** with no Core Audio client running, coreaudiod is classified as a Darwin background process and the kernel coalesces its timers by up to 100 ms (`kern.timer_coalesce_bg_ns_max`). A 1 ms send loop then leaves the wire in bursts of about a dozen packets with 15-37 ms stalls, while the average rate stays exactly right.
    - **What didn't work:** deadline (time-constraint) scheduling and an `NSActivityLatencyCritical` activity.
    - **What did:** clearing the Darwin background classification (`setpriority(PRIO_DARWIN_PROCESS/THREAD, 0, 0)`).
    - **Why it matters now:** our TX runs without IO, so it applies to us.
    - **The plan:** measure first. If needed, clear the classification only while at least one TX stream exists, and restore it when none does. The fork held a latency-critical activity from plug-in load forever, for every user.
  - **Anchor the send schedule inside the TX thread.** Anchoring before the thread started cost the fork 3.2 ms of constant lateness. Done here: the transmitter anchors in its loop.
  - **Re-anchor on a media-time discontinuity; never slew across it.** The fork used `mediaTicks_ == 0` as "unanchored" while also counting with it. A stream started before PTP lock never anchored, then sent about 1.45x too fast indefinitely. Done here: a generation change re-anchors, and positions never repeat.
  - **Acceptance targets from the fork's measurements:**
    - packets leave within one packet of their media time
    - none miss a 2 ms Dante receive window over 120,000 packets
    - the stream is stable across driver restarts
  - **`ts-refclk` must follow the live grandmaster.** Bump the SDP version and re-announce when the grandmaster changes.
- Pending:
  - **The real HAL.** In particular, measure the background coalescing above while no client is running.
  - **PTP-derived timestamps**, with `ts-refclk:ptp=` and a real `mediaclk` offset (phase 4).
  - **Criterion 4** with a Dante or RAVENNA receiver.

### Phase 6 (optional): ASRC for foreign-clock streams

## Spikes (do first)

- **S1, PTP inside the sandbox: resolved, PTP runs in-process.** Inside the driver host (`_coreaudiod`, macOS 26.0.1) the probe bound UDP 319/320, joined 224.0.1.129, sent, and received with kernel monotonic timestamps, with or without `AudioServerPlugIn_Network`. Host threads were scheduled up to 31.7 ms late, so kernel timestamps are mandatory. Details: [Spikes/S1-PTP-Sandbox.md](Spikes/S1-PTP-Sandbox.md).
- **S2, HAL acceptance: resolved, Core Audio follows the model exactly.** With the Raw algorithm and a 16384-frame period, the measured rate matched a +300 ppm model to 0.1 ppm, tracked a +300 to -300 ppm ramp within 3 ppm (re-anchoring every 100 ms), and a phase step with a new seed produced exactly one clean timeline jump with no overloads in 3284 IO cycles. Core Audio adds no smoothing under Raw, so the servo must publish a smooth rate and slew small phase errors rather than step. Details: [Spikes/S2-HAL-Clock.md](Spikes/S2-HAL-Clock.md).
- **S3, PTP accuracy.** Log offset and path-delay statistics against a real grandmaster to set link offset defaults and lock thresholds. On the target network this means the network's grandmaster, not the Riedel (a TimeReceiver). Also measure the actual Sync, Announce and Delay_Resp rates, and confirm the master answers unicast Delay_Req (hybrid mode).

## Prior work reviewed

marcnnn's fork (`feat/aes67-wing-interop`, 33 commits, September 2026) was reviewed in October 2026, with each commit compared against step 1 and step 2 and three reviews covering PTP, transmit and SAP, and device and streams. It was tested against a Behringer WING, and its measurements are recorded in phases 4 and 5 above.

- **Ported** (rewritten against this design, with tests, credited):
  - SDP fixes: `ts-refclk` forms, origin order, SAP payload type (#11)
  - Release-build test fix and bundle signing (#11)
  - SAP announcements, with jitter, stable session IDs, per-stream interface and an honest `ts-refclk` (#14)
  - the Dante AES67 prefix warning (#14)
  - separate input and output channel allocation (#15)
- **Not ported:**
  - **The PTP slave and helper agent:** the defects and security issues in phase 4's lessons.
  - **Transmit timing:** superseded by phase 5's media-paced transmitter; its startup bug is described above.
  - **Clock steering from buffer fill:** superseded by phase 3. Its gains assumed one update per second where the HAL calls once per IO cycle, an underrun threw the lock away, and an unread buffer could pin the clock at its limit.
  - **One Core Audio device per configured device:** a good fit for a per-device `MediaClock`, but saving from one device deleted the other devices' streams. Left for later, with the save path fixed.

## Risks

- **Software timestamp asymmetry biases phase.** Mitigation: minimum-delay filtering, a calibration offset, and a generous default link offset.
- **Other PTP software on the same Mac** (another AoIP driver) contends for ports 319/320. Mitigation: `SO_REUSEPORT` and a documented limitation.
- **Sample rate changes mid-stream.** Mitigation: rebase `mediaBase`, flush buffers, bump the seed.
- **Background timer coalescing delays continuous TX.** Measured in the real HAL (#201): with no client running IO, Core Audio puts the driver's host process at priority 4 and the transmit thread loses its time-constraint scheduling, so mach_wait_until wakes 30-100 ms late and only about 50 of 1000 packets a second went out. The process can't leave that band itself (task role, QoS override, PRIO_DARWIN_PROCESS and an NSProcessInfo latency-critical activity all had no effect). The transmit thread now waits on a NOTE_CRITICAL kqueue timer, which the kernel doesn't coalesce: idle sending is 970-995 packets a second. The rest comes from running at priority 4 and needs an IO client to remove.
- **Scope.** This is the largest change in the project. Mitigation: phased PRs, each independently shippable, with deterministic virtual-time tests so timing does not make the suite flaky.

## Decisions needed

1. **Default link offset: decided, 8 x packet time (8 ms at 1 ms packets), configurable 0.25-20 ms.** Matches the target network's receive buffer convention, favouring robustness on a busy network; revisit after spike S3 measures jitter and PTP accuracy.
2. **Stream-recovered mode (Phase 3).** Recommended, because it delivers drift-free RX from the Riedel before PTP lands.
3. **PTP placement.** Resolved by spike S1: in-process.
4. **ASRC.** Defer to Phase 6, choose the library then.
5. **Tracking.** Whether to track phases as GitHub issues under a milestone.
