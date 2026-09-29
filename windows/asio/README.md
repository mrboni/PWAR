# Local Windows lifecycle fix

This fork fixes two hangs seen in Ableton Live when a PWAR peer is absent:

- Driver shutdown used to join a thread blocked indefinitely in `WSARecvFrom`.
- Starting PWAR without incoming packets delivered no ASIO callbacks, causing
  Live's audio command queue to time out.

The receiver now waits on a socket event and a separate stop event. `stop()`,
buffer disposal and destruction join the receiver before releasing its buffers
or sockets. The receiver only runs between ASIO start and stop. Socket setup
errors are returned to the host, and the sender is nonblocking.

When no complete network buffer has arrived for 100 ms, the same thread supplies
silent input callbacks on a local timer. It also does this immediately on an
offline start. Incoming complete buffers resume network-driven callbacks, so an
active relay still controls the clock. The fallback keeps the DAW responsive; it
does not make a disconnected Pi audible. The wire format and Pi configuration
are unchanged.

## Build and test

Requires Visual Studio C++ tools, a Windows SDK, CMake and the Steinberg ASIO SDK.
Download ASIO SDK 2.3.4 from [Steinberg](https://www.steinberg.net/developers/) and
extract it into `third_party/asiosdk`, or pass `-DASIOSDK_DIR=C:/path/to/ASIOSDK`.
The SDK remains a separate dependency and is not committed in this fork.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The DLL is `build/windows/asio/Release/PWARASIO.dll`. Its ASIO driver version is
0.2.1 (a local build, not an upstream release).

Tests load the actual DLL through COM/ASIO, with a private configuration and
loopback packets. Close or deselect PWAR in other applications before testing;
the tests require exclusive use of UDP 8321. Coverage includes offline startup
at 44.1/48 kHz and multiple buffer sizes, both callback APIs, a synthetic incoming
stream, disconnect/reconnect, invalid and incomplete packets, active buffer
disposal, 50 repeated lifecycles, callback quiescence, handle leaks, and bind
failure followed by a successful retry. Physical Pi audio remains an integration
test to perform with the real relay.

## Install for the current user

For a machine where PWAR is already registered, the script below copies the DLL
into a stable directory and overrides only PWAR's COM server path for the current
user. The original DLL and machine-wide registration remain available. Close
and reopen the DAW after installation because an existing process can cache its
old COM class factory/DLL. Save any unsaved set first.

```powershell
./scripts/Install-LocalPwar.ps1 -SourceDll ./build/windows/asio/Release/PWARASIO.dll
```

The script records the previous registration and a DLL hash in `installation.json`.
Use `./scripts/Install-LocalPwar.ps1 -Restore` to restore the previous driver path.
The existing `%USERPROFILE%/pwarASIO.cfg` is retained.
