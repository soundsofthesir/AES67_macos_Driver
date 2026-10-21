# Spike S4: where the driver can keep its config (#42)

## Question

The driver runs as `_coreaudiod` inside Apple's sandboxed Core Audio driver host. Stream changes made at runtime never reach `streams.json` (#42). Is that only because postinstall makes `/Library/Application Support/AES67Driver` root-owned, or does the sandbox stop the driver writing there at all? If files are out, does the host's own storage (`WriteToStorage`, `aspl::Storage`) work instead?

Since #74 a save is a temp file in the config directory, flushed and renamed over `streams.json`, so the driver needs to create files in the directory, not just write to `streams.json`.

## Method

`Tools/StorageProbe/` builds a throwaway HAL plug-in that publishes no device and uses no network (CMake option `BUILD_SPIKES=ON`, target `AES67StorageProbe`). When the host initialises it, it logs PASS/FAIL under subsystem `com.aes67driver.probe` for:

- in each of `/Library/Application Support/AES67Driver` (root:wheel 755, as postinstall makes it), `/Library/Application Support/AES67Driver-owned` (`_coreaudiod`, the fix proposed in #42), `/Library/Application Support`, the host's `HOME`, its temp directory and `/tmp`: read and open-for-writing an existing `streams.json`, create a temp file, write, `F_FULLFSYNC`, rename, remove, and mkdir;
- the host's storage: write a 64 KB string, read it back, delete it.

To reproduce (one coreaudiod restart):

1. Configure with `-DBUILD_SPIKES=ON` and build `AES67StorageProbe`.
2. Create both directories (each with a small `streams.json`), owned as above.
3. Copy the bundle to `/Library/Audio/Plug-Ins/HAL/` and restart coreaudiod.
4. Read the results with `sudo log show --last 5m --info --debug --predicate 'subsystem == "com.aes67driver.probe"'`.
5. Remove the bundle and both directories. The probe stays loaded, with no device, until coreaudiod next restarts.

## Results

Run on 2026-10-20, macOS 26.0.1. Host process `Core Audio Driver (AES67StorageProbe.driver)`, uid 202 (`_coreaudiod`), `HOME=/var/empty`.

| Location | Owner, mode | Read streams.json | Write it in place | Temp file, write, F_FULLFSYNC, rename | mkdir |
|---|---|---|---|---|---|
| `/Library/Application Support/AES67Driver` | root:wheel 755 (postinstall today) | PASS | EACCES | EACCES | EACCES |
| `/Library/Application Support/AES67Driver-owned` | _coreaudiod:wheel 755 | PASS | PASS | PASS | PASS |
| `/Library/Application Support` | root:admin 755 | - | - | EACCES | EACCES |
| `/var/empty` (the host's HOME) | root:sys 755 | - | - | EPERM | EPERM |
| the host's temp directory (`/var/folders/zz/.../T/`) | _coreaudiod 700 | - | - | PASS | PASS |
| `/tmp` | root:wheel 1777 | - | - | PASS | PASS |

Host storage (`aspl::Storage`): writing a 64 KB string, reading it back and deleting it all PASS.

## Conclusions

1. **#42 is file ownership, not the sandbox.** The root-owned directory fails with EACCES; owned by `_coreaudiod`, every step of the save works, rename included. The fix is postinstall giving the directory to `_coreaudiod`, on every install so older installs are fixed too.
2. The only sandbox denial (EPERM) was the host's home, `/var/empty`. So the user-level search path (`~/Library/Application Support/AES67Driver/`) never applies to the installed driver.
3. `/tmp` is writable from the host. The old file log was not blocked by the sandbox; the comment in `Driver/DebugLog.h` said it was.
4. Host storage works, so it stays an option if a later macOS closes off `/Library/Application Support`. Files are kept for now: `streams.json` stays readable and editable, and the manager app can find it.

## End-to-end check of the fix

2026-10-21: the built driver was copied into place and the real `Installer/scripts/postinstall` run, with no `streams.json` and no `ptp.json`. The driver added its receive-only test stream and saved it: the directory and `streams.json` were both `_coreaudiod:wheel`, the file held the one test stream, and the log showed `Saved 1 stream configurations (+0 not loaded)`. Driver and directory were removed afterwards.

An en0 capture of everything this Mac sent to 239/8 during the run held two SSDP packets (239.255.255.250:1900, TTL 1) and nothing else. The driver has no SSDP code; another process sends these. To attribute packets to processes next time, capture with `tcpdump -i pktap,en0 -k`.

## Notes on running the probe

The first run's log entries showed as `<compose failure>`: `log show` needs the logging binary's format strings, and the run had already removed the bundle. The probe now also writes its results to `/tmp/aes67-storage-probe.txt`. Also, in zsh `log` is a builtin: scripts must call `/usr/bin/log`.
