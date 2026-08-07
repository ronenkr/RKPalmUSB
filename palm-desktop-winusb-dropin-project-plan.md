# Palm Desktop WinUSB Drop-In Transport

## Project Plan

**Document version:** 1.0  
**Date:** 2026-08-04  
**Primary platform:** Windows 11 x64 with Memory Integrity enabled  
**Primary deliverable:** A drop-in, 32-bit `USBPort.dll` replacement for Palm Desktop, backed by Microsoft WinUSB rather than the obsolete Palm/Aceeca kernel driver

---

## 1. Executive Summary

This project will create a modern USB transport layer that allows Palm Desktop and HotSync Manager to synchronize with USB Palm OS devices on Windows 11 without disabling Memory Integrity.

The compatibility boundary will be the user-mode `USBPort.dll` loaded by Palm Desktop. The replacement DLL will reproduce the original DLL's exported ABI and observable behavior while internally communicating with the Palm through Microsoft's in-box `winusb.sys` driver.

```text
Palm Desktop / HotSync Manager (32-bit)
                 |
                 | original USBPort.dll ABI
                 v
       Replacement USBPort.dll (x86)
                 |
                 | WinUSB user-mode API
                 v
          Microsoft winusb.sys
                 |
                 v
          Palm OS USB device
```

The project intentionally avoids implementing a new kernel-mode USB driver. The only kernel USB function driver will be Microsoft's signed `winusb.sys`. The project will supply a device-installation package that associates supported Palm USB hardware IDs with WinUSB.

The critical path is not USB transfer code. It is accurately reconstructing the ABI and behavioral contract between Palm Desktop/HotSync Manager and the original `USBPort.dll`.

---

## 2. Project Goals

### 2.1 Primary goals

1. Provide a drop-in `USBPort.dll` replacement that Palm Desktop can load without modifying Palm Desktop binaries.
2. Synchronize at least one USB Palm OS device successfully on Windows 11 x64.
3. Keep Windows Memory Integrity, Secure Boot, and normal driver-signing enforcement enabled.
4. Use Microsoft `winusb.sys` instead of the obsolete Palm or Aceeca kernel driver.
5. Preserve normal Palm Desktop functionality, including conduits, backup, restore, and PRC/PDB installation.
6. Handle Palm USB arrival, HotSync-button activation, disconnect, cancellation, and repeated synchronization sessions reliably.
7. Produce a maintainable compatibility framework that can add more Palm, Handspring, Sony, Fossil, and Aceeca hardware IDs later.

### 2.2 Secondary goals

1. Support multiple Palm Desktop releases through DLL ABI profiles.
2. Provide diagnostic logging suitable for reverse engineering and field support.
3. Provide a command-line USB probe independent of Palm Desktop.
4. Build a replayable test corpus from real HotSync sessions.
5. Document the original `USBPort.dll` ABI and the Palm-side USB transport sufficiently for future maintenance.

---

## 3. Non-Goals

The first release will not attempt to:

- Replace Palm Desktop itself.
- Reimplement Palm Desktop conduits.
- Create a virtual serial port unless later evidence shows Palm Desktop requires one.
- Preserve the private IOCTL interface of the obsolete Aceeca kernel driver.
- Support every Palm OS USB device in the initial milestone.
- Support infrared, Bluetooth, network HotSync, or physical serial cradles in the initial milestone.
- Modify Palm ROMs or device firmware.
- Circumvent Windows driver-signing or security policies.
- Patch the original Palm/Aceeca kernel driver binary.

---

## 4. Definition of “Drop-In Replacement”

The replacement is considered drop-in compatible when all of the following are true:

- The file is named `USBPort.dll`.
- It is installed in the location from which HotSync Manager expects to load it.
- It has the same PE architecture as the original DLL, expected to be x86.
- It exposes all required export names and ordinals.
- It uses the correct calling conventions.
- It accepts the same parameter layouts and callbacks.
- It returns compatible status and error values.
- It follows the expected initialization, listen, open, read, write, cancel, close, and shutdown sequence.
- Palm Desktop requires no binary patching.
- Existing conduits continue to run normally.
- The obsolete Palm/Aceeca `.sys` driver is not installed or loaded.

A one-time installation of the WinUSB device package is acceptable. “Drop-in” applies to the Palm Desktop transport DLL interface, not to Windows Plug and Play configuration.

---

## 5. Target Configuration

The project must lock one exact reference configuration before implementation begins.

### 5.1 Initial reference target

Record the following in `docs/reference-target.md`:

| Item | Required value |
|---|---|
| Windows edition and build | Exact Windows 11 build |
| CPU architecture | x64 |
| Memory Integrity | Enabled |
| Secure Boot | Enabled |
| Palm Desktop version | Exact version and installer source |
| `HOTSYNC.EXE` SHA-256 | Exact hash |
| Original `USBPort.dll` SHA-256 | Exact hash |
| Original USB driver package | Exact INF, CAT, SYS, and DLL files |
| Palm device model | Exact model |
| Palm OS version | Exact version |
| USB VID/PID | Normal and HotSync-mode IDs if different |
| Known-good legacy host | Windows version on which original driver works |

### 5.2 Expansion targets

After the reference target passes all acceptance tests, add targets one at a time:

- Additional Palm Desktop versions.
- Additional Palm-branded USB devices.
- Handspring Visor and Treo devices.
- Sony CLIÉ devices.
- Fossil Wrist PDA devices.
- Aceeca devices.
- Devices with composite USB configurations or model-specific framing.

---

## 6. Architecture

### 6.1 Preferred architecture

```text
+-------------------------------------------------------------+
| Palm Desktop / HotSync Manager                              |
| 32-bit process                                              |
+-----------------------------+-------------------------------+
                              |
                              | Original USBPort.dll ABI
                              v
+-------------------------------------------------------------+
| Replacement USBPort.dll                                    |
|                                                             |
|  +--------------------+   +------------------------------+  |
|  | ABI compatibility  |   | Session/state controller     |  |
|  | - exports          |   | - listen                     |  |
|  | - callbacks        |   | - connect                    |  |
|  | - error mapping    |   | - transfer                   |  |
|  | - structure layout |   | - cancel/disconnect          |  |
|  +--------------------+   +------------------------------+  |
|                 |                    |                       |
|                 +----------+---------+                       |
|                            v                                 |
|                 WinUSB transport backend                    |
|                 - SetupAPI enumeration                      |
|                 - CreateFile                                |
|                 - WinUsb_Initialize                         |
|                 - endpoint discovery                        |
|                 - asynchronous read/write                   |
|                 - timeout/cancel                            |
+----------------------------+--------------------------------+
                             |
                             v
                    Microsoft winusb.sys
                             |
                             v
                      Palm USB device
```

### 6.2 Optional out-of-process architecture

Use this only if in-process constraints make the DLL unstable or too complex:

```text
HOTSYNC.EXE
    |
    v
Thin x86 USBPort.dll shim
    |
    | named pipe or local RPC
    v
PalmUsbHost.exe
    |
    v
WinUSB
```

Advantages:

- Driver and protocol failures cannot corrupt HotSync Manager as easily.
- The host process can be 64-bit while the shim remains 32-bit.
- Logging, recovery, and device ownership become simpler.
- The transport host can be restarted independently.

Disadvantages:

- Extra IPC latency and failure modes.
- More packaging and lifecycle management.
- Callback and cancellation behavior may be harder to reproduce.

The in-process DLL is the default. Move out of process only when evidence justifies it.

---

## 7. Core Design Principles

1. **Measure before implementing.** Do not guess DLL prototypes, IOCTL semantics, USB framing, or timing.
2. **Preserve the Palm-facing ABI, not the obsolete driver-facing ABI.**
3. **Use Microsoft’s USB stack.** Avoid custom kernel code unless WinUSB is conclusively insufficient.
4. **Separate ABI emulation from USB transport.** The Palm-facing layer must not directly contain WinUSB-specific logic.
5. **Use captured sessions as regression data.** Every discovered behavior should become a trace or automated test.
6. **Fail safely.** Disconnects, duplicate HotSync presses, and Palm resets must not crash or hang Palm Desktop.
7. **Keep the initial scope narrow.** One Palm Desktop version and one device must work end-to-end before broad compatibility work begins.
8. **Preserve observability.** Every state transition and OS error should be traceable without exposing personal Palm database contents by default.

---

## 8. Major Unknowns

The following must be resolved experimentally:

1. Whether HotSync Manager imports `USBPort.dll` statically or loads it with `LoadLibrary`.
2. The complete export table, including ordinals and decorated names.
3. Exact calling conventions and argument layouts.
4. Whether the original DLL registers callbacks into HotSync Manager.
5. Whether HotSync Manager expects a byte stream, packet messages, or a higher-level transport API.
6. Whether the DLL performs Palm USB framing or delegates framing to the kernel driver.
7. Which device-interface GUID and registry values the original DLL expects.
8. Whether Palm Desktop expects one long-lived listener or repeatedly creates transport objects.
9. Whether different Palm Desktop releases use different DLL ABIs.
10. Whether different Palm models expose different endpoint layouts or handshake protocols.
11. Whether the device changes VID/PID or interfaces after the HotSync button is pressed.
12. Whether any supported device requires a kernel feature unavailable through WinUSB.

These unknowns define the project’s research backlog and must not be hidden behind assumptions.

---

## 9. Workstreams

The project is divided into eight workstreams. Several can proceed in parallel after the reference environment is captured.

### Workstream A — Reference laboratory

Build reproducible legacy and modern test systems.

### Workstream B — DLL ABI discovery

Recover the exact contract between HotSync Manager and `USBPort.dll`.

### Workstream C — Original-driver behavior tracing

Observe how the original DLL talks to the Palm/Aceeca kernel driver.

### Workstream D — Palm USB transport research

Determine device enumeration, endpoints, framing, handshake, and session rules.

### Workstream E — WinUSB package and backend

Bind supported hardware to WinUSB and implement reliable asynchronous USB I/O.

### Workstream F — Drop-in DLL implementation

Replace proxy forwarding with native compatibility behavior backed by WinUSB.

### Workstream G — Testing and compatibility expansion

Verify Palm Desktop conduits and add devices incrementally.

### Workstream H — Packaging, signing, and release

Produce a safe installer, rollback path, documentation, and signed device package.

---

## 10. Phase Plan

## Phase 0 — Preserve Inputs and Establish the Lab

### Objectives

- Preserve every binary and configuration involved in a known-good HotSync.
- Create a legacy reference host and a Windows 11 development host.
- Ensure every future result is tied to exact binary hashes.

### Tasks

1. Archive the original Palm Desktop installer.
2. Archive the full Palm/Aceeca driver package:
   - INF
   - CAT
   - SYS
   - `USBPort.dll`
   - helper DLLs
3. Hash all binaries with SHA-256.
4. Record PE architecture, timestamp, version resources, imports, and exports.
5. Export relevant registry keys from the known-good installation.
6. Capture the installed device instance, hardware IDs, compatible IDs, services, interfaces, and driver stack.
7. Create a Windows 7 or Windows 10 VM or physical reference system where USB HotSync completes successfully.
8. Create a Windows 11 x64 development system with:
   - Secure Boot enabled
   - Memory Integrity enabled
   - Visual Studio with C++ desktop tools
   - Windows SDK
   - WinDbg
   - Ghidra
   - x32dbg
   - Process Monitor
   - USBPcap and Wireshark
9. Store a clean VM snapshot before each driver experiment.

### Suggested commands

```bat
certutil -hashfile HOTSYNC.EXE SHA256
certutil -hashfile USBPort.dll SHA256

dumpbin /headers USBPort.dll > USBPort.headers.txt
dumpbin /exports USBPort.dll > USBPort.exports.txt
dumpbin /imports USBPort.dll > USBPort.imports.txt
dumpbin /imports HOTSYNC.EXE > HOTSYNC.imports.txt

pnputil /enum-devices /connected /deviceids /drivers /interfaces /stack
pnputil /enum-drivers /files
```

### Deliverables

- `evidence/binaries/`
- `evidence/hashes.sha256`
- `docs/reference-target.md`
- `docs/legacy-installation.md`
- `docs/device-identities.md`
- Reproducible successful legacy HotSync capture

### Exit criteria

- A full HotSync can be repeated on the reference host.
- All involved binaries and IDs are archived and hashed.
- The Windows 11 host starts with Memory Integrity enabled and no obsolete Palm kernel driver loaded.

---

## Phase 1 — Recover the `USBPort.dll` ABI

### Objectives

- Determine exactly how HotSync Manager loads and calls `USBPort.dll`.
- Produce a machine-readable ABI manifest.
- Avoid implementing USB until the Palm-facing contract is understood.

### Static-analysis tasks

1. Extract all exports and ordinals.
2. Identify decorated x86 names and inferred calling conventions.
3. Inspect imports to identify SetupAPI, registry, synchronization, serial, or driver calls.
4. Search HotSync Manager for:
   - `USBPort.dll`
   - export names
   - `LoadLibraryA/W`
   - `GetProcAddress`
   - device descriptions
   - registry paths
5. Decompile the original DLL in Ghidra.
6. Identify global state, exported entry points, callbacks, object layouts, and state transitions.
7. Document every probable function prototype with a confidence rating.

### Dynamic-analysis tasks

1. Break on `LoadLibraryA`, `LoadLibraryW`, and `GetProcAddress` in x32dbg.
2. Record all requested DLL names, export names, and ordinals.
3. Break on every exported function.
4. Log:
   - caller address
   - thread ID
   - register state
   - stack arguments
   - input buffers
   - output buffers
   - return values
   - `GetLastError()` values
5. Repeat traces for:
   - HotSync Manager startup
   - HotSync button press
   - successful synchronization
   - user cancellation
   - device unplug during transfer
   - second synchronization without restarting HotSync Manager
   - HotSync Manager exit

### ABI manifest format

Create `abi/usbport-<hash>.yaml`:

```yaml
dll_sha256: "..."
architecture: x86
exports:
  - ordinal: 1
    name: "ExactExportName"
    calling_convention: unknown
    stack_bytes: null
    arguments:
      - type: unknown
        direction: unknown
    return_type: unknown
    observed_calls: []
    confidence: low
```

### Deliverables

- Exact export `.def` file
- ABI manifest
- Ghidra project or exported analysis notes
- Call-sequence diagrams
- Initial state-machine model

### Exit criteria

- Every export used by HotSync Manager is identified.
- Calling conventions are known or safely captured by forwarding wrappers.
- The startup-to-shutdown call sequence is documented.
- Unknown structure fields are explicitly marked rather than guessed.

---

## Phase 2 — Build an Exact Proxy DLL

### Objectives

- Prove that a replacement DLL can be loaded as a true drop-in.
- Observe calls without changing behavior.
- Establish a safe platform for incremental replacement.

### Design

```text
HOTSYNC.EXE
    |
    v
Replacement USBPort.dll
    |-- trace call
    |-- normalize logging
    |-- forward call
    v
USBPort.original.dll
```

### Tasks

1. Rename the original DLL to a private, absolute filename such as `USBPort.original.dll`.
2. Build an x86 proxy with the exact original export names and ordinals.
3. Forward all calls to the original DLL.
4. Preserve:
   - calling convention
   - stack balance
   - return values
   - `GetLastError()` when semantically required
   - callback pointers
5. Implement bounded diagnostic logging.
6. Avoid complex work in `DllMain`.
7. Add a loader guard to prevent recursive self-loading.
8. Add a switch to disable logging for behavior comparison.
9. Confirm that a complete HotSync succeeds through the proxy.

### Logging requirements

Log metadata by default, not Palm database content:

- monotonic timestamp
- process ID
- thread ID
- function name/ordinal
- state before and after
- buffer length
- return code
- Windows error code

Payload logging must be a separate opt-in diagnostic mode.

### Deliverables

- `USBPort.proxy.dll`
- Exact exports `.def`
- Proxy trace logs
- Automated export-comparison tool
- Proxy installation and rollback script

### Exit criteria

- Palm Desktop loads the proxy DLL.
- A successful HotSync completes through the proxy and original backend.
- Repeated sessions behave identically with and without proxy logging.
- Export names, ordinals, and architecture match automatically in CI.

---

## Phase 3 — Trace the Original Driver Boundary

### Objectives

- Determine the semantic operations performed by the original `USBPort.dll`.
- Separate Palm-facing API behavior from obsolete-driver implementation details.

### Hook targets

Trace or break on:

```text
SetupDiGetClassDevsW
SetupDiEnumDeviceInterfaces
SetupDiGetDeviceInterfaceDetailW
SetupDiGetDeviceRegistryPropertyW
CM_Get_Device_Interface_ListW
CreateFileW
DeviceIoControl
ReadFile
WriteFile
GetOverlappedResult
CancelIo
CancelIoEx
WaitForSingleObject
WaitForMultipleObjects
CreateEventW
RegisterDeviceNotificationW
CloseHandle
RegOpenKeyExW
RegQueryValueExW
```

### Data to capture

- Device-interface class GUIDs.
- Device path formats.
- Access flags and sharing modes.
- Whether handles are opened with `FILE_FLAG_OVERLAPPED`.
- IOCTL numbers and transfer methods.
- Input/output buffer sizes and contents.
- Read/write request sizes.
- Timeout behavior.
- Cancellation order.
- Error codes on disconnect.
- Whether the kernel driver exposes a byte stream or packet interface.
- Whether the DLL or kernel driver performs Palm USB framing.

### Tooling

Use a combination of:

- x32dbg breakpoints and scripts
- API Monitor or equivalent local instrumentation
- Process Monitor
- WinDbg on the reference VM if required
- USBPcap/Wireshark for bus-level correlation
- A custom proxy hook layer where generic tools are insufficient

### Deliverables

- `docs/original-driver-contract.md`
- IOCTL manifest
- API-to-USB correlation table
- Session timeline aligning DLL calls, driver calls, and USB packets

### Exit criteria

- The minimum semantic transport interface required by the replacement DLL is known.
- It is clear which original driver details can be discarded.
- There is sufficient evidence to design the WinUSB state machine.

---

## Phase 4 — Characterize Palm USB Devices

### Objectives

- Build a reliable hardware and protocol profile for the initial target device.
- Determine whether pilot-link behavior can serve as a reference implementation or test oracle.

### Enumeration tasks

Capture for every observed device state:

- VID and PID
- `bcdDevice`
- manufacturer, product, and serial strings
- configuration count
- interface number
- interface class/subclass/protocol
- endpoint addresses
- endpoint transfer types
- maximum packet sizes
- USB speed
- composite-device layout
- IDs before and after pressing HotSync

### Protocol tasks

1. Capture a known-good HotSync with USBPcap.
2. Identify control transfers during device initialization.
3. Identify bulk or interrupt IN/OUT endpoints.
4. Determine the first host and device messages.
5. Establish packet framing, lengths, sequence fields, checksums, and acknowledgements.
6. Determine idle/listen behavior.
7. Test timeout behavior when the HotSync button is not pressed.
8. Test unplug, reset, and cancelled session behavior.
9. Compare captures with pilot-link source and runtime traces.
10. Decide whether to:
    - implement only the Palm USB framing needed by Palm Desktop, or
    - reuse a suitably licensed pilot-link component behind a narrow adapter.

### Licensing checkpoint

Before copying pilot-link code:

- Identify the exact source files under consideration.
- Record each file’s license.
- Confirm obligations for static or dynamic linking.
- Prefer behavioral reference or clean-room reimplementation if licensing is unclear.

### Deliverables

- `devices/<model>.yaml`
- Sanitized USBPcap traces
- Packet-decoder notes
- Initial protocol parser
- Device-profile schema

### Exit criteria

- A standalone tool can recognize the target Palm and locate the correct endpoints.
- The initial handshake is understood well enough to reproduce.
- Disconnect and repeated-connect behavior are documented.

---

## Phase 5 — Implement the WinUSB Device Package and Probe

### Objectives

- Bind the initial device to Microsoft `winusb.sys`.
- Open and communicate with it outside Palm Desktop.
- Validate that Memory Integrity remains enabled.

### WinUSB package tasks

1. Create a dedicated device-interface GUID.
2. Write a Universal INF that references the in-box WinUSB driver.
3. Add only verified hardware IDs.
4. Use the `USBDevice` setup class unless testing reveals a stronger requirement.
5. Generate and sign the catalog appropriately for each release stage.
6. Provide install, uninstall, and rollback scripts using PnPUtil.
7. Verify driver ranking against any installed Palm/Aceeca package.
8. Confirm that `winusb.sys` is the active function driver.

### Probe application tasks

Create `palm-usb-probe.exe` with commands such as:

```text
palm-usb-probe list
palm-usb-probe descriptors
palm-usb-probe endpoints
palm-usb-probe listen
palm-usb-probe handshake
palm-usb-probe capture <file>
palm-usb-probe replay <file>
```

The probe must:

- enumerate through SetupAPI using the interface GUID;
- open the device with `CreateFile` and `FILE_FLAG_OVERLAPPED`;
- call `WinUsb_Initialize`;
- discover endpoints dynamically;
- configure pipe policies explicitly;
- perform asynchronous reads and writes;
- support cancellation;
- detect surprise removal;
- emit structured logs.

### Deliverables

- `PalmWinUSB.inf`
- Development driver package
- `palm-usb-probe.exe`
- WinUSB transport library
- Device enumeration and endpoint tests

### Exit criteria

- Windows Device Manager shows the device using `winusb.sys`.
- Memory Integrity remains enabled.
- The probe opens the device and completes the initial Palm handshake.
- Unplug and reconnect do not require restarting the probe.

---

## Phase 6 — Implement the Native Drop-In DLL

### Objectives

- Replace original-driver forwarding with the native WinUSB backend.
- Preserve the Palm Desktop-facing behavior discovered earlier.

### Module layout

```text
usbport/
├── dllmain.cpp
├── exports.def
├── abi/
│   ├── abi_profile.h
│   ├── abi_profile_<hash>.cpp
│   ├── argument_validation.cpp
│   └── error_mapping.cpp
├── session/
│   ├── session_controller.cpp
│   ├── state_machine.cpp
│   ├── callbacks.cpp
│   └── cancellation.cpp
├── transport/
│   ├── transport.h
│   ├── winusb_transport.cpp
│   ├── device_enumerator.cpp
│   ├── pipe_policy.cpp
│   └── async_request.cpp
├── protocol/
│   ├── palm_usb_framing.cpp
│   ├── packet_parser.cpp
│   └── device_profile.cpp
└── diagnostics/
    ├── logger.cpp
    ├── trace_events.cpp
    └── redaction.cpp
```

### Session state machine

Use explicit states rather than scattered Boolean flags:

```text
Uninitialized
    -> Initialized
    -> Listening
    -> DevicePresent
    -> Opening
    -> Handshaking
    -> Connected
    -> Transferring
    -> Closing
    -> Listening

Any active state
    -> Cancelling
    -> Closing
    -> Listening or Shutdown

Any state
    -> Faulted
    -> Closing
    -> Listening or Shutdown
```

Every transition must define:

- triggering event
- permitted source states
- side effects
- callback behavior
- return code
- cleanup action
- retry policy

### Threading model

Recommended initial model:

- One transport worker thread per active Palm session.
- One synchronized session object.
- Overlapped WinUSB requests.
- Explicit cancellation event.
- No blocking device enumeration inside `DllMain`.
- Lazy initialization on first exported call.
- Bounded shutdown wait.
- No callbacks while holding internal locks.

### Compatibility requirements

1. Match exact export names and ordinals.
2. Match all observed return codes.
3. Preserve callback thread expectations where known.
4. Replicate blocking or non-blocking behavior.
5. Map Windows errors to Palm transport errors centrally.
6. Handle zero-length reads and writes consistently.
7. Reject invalid states deterministically.
8. Make close and cancel idempotent.
9. Recover after unplug without restarting HotSync Manager.
10. Support consecutive HotSync sessions.

### Incremental replacement strategy

Do not replace all exports at once.

1. Keep the proxy architecture.
2. Select one export or semantic operation.
3. Implement it natively.
4. Continue forwarding all others.
5. Compare original and native traces.
6. Add regression tests.
7. Continue until the original DLL is no longer loaded.

This reduces the debugging search space and preserves a working reference throughout development.

### Deliverables

- Native x86 `USBPort.dll`
- ABI profile for the reference Palm Desktop version
- WinUSB transport backend
- Palm USB framing layer
- Structured diagnostic logs
- Native/proxy comparison tests

### Exit criteria

- Palm Desktop detects the Palm through the replacement DLL.
- A full HotSync completes with the original DLL and obsolete SYS removed.
- Palm Desktop remains usable after unplug, cancellation, and repeated sessions.

---

## Phase 7 — Functional and Compatibility Testing

### 7.1 Core functional tests

- HotSync Manager starts with no Palm attached.
- Pressing the Palm HotSync button starts a session.
- User name and device identity are read correctly.
- System conduit runs.
- Backup conduit runs.
- PRC installation succeeds.
- PDB installation succeeds.
- Modified desktop records sync to Palm.
- Modified Palm records sync to desktop.
- Conflict resolution behaves normally.
- Restore operation succeeds on a test device.
- HotSync log reports normal results.

### 7.2 Lifecycle tests

- Connect before HotSync Manager starts.
- Connect after HotSync Manager starts.
- Press HotSync repeatedly.
- Cancel from Palm.
- Cancel from desktop.
- Unplug during idle.
- Unplug during handshake.
- Unplug during transfer.
- Reset Palm during transfer.
- Exit HotSync Manager while listening.
- Exit HotSync Manager during an active session.
- Run ten consecutive synchronizations without restarting either system.
- Sleep and resume Windows.
- Switch Windows user sessions.

### 7.3 Data-integrity tests

Use disposable test profiles and known datasets:

- empty Palm profile
- small address book
- large address book
- recurring calendar events
- records containing non-ASCII text
- maximum-length memo records
- deleted-record synchronization
- duplicate-record scenarios
- large PRC/PDB files within device limits

Before and after every test:

- hash source files;
- export Palm databases;
- compare record counts;
- compare record IDs where meaningful;
- retain HotSync logs;
- retain transport traces.

### 7.4 Security and platform tests

Verify:

- Secure Boot enabled.
- Memory Integrity enabled.
- Microsoft vulnerable-driver blocklist active under the system configuration.
- No obsolete Palm/Aceeca kernel driver loaded.
- No test-signing mode in production tests.
- No writable/executable memory required by the DLL.
- DLL search paths do not permit trivial preloading attacks.
- Logs are written to a controlled per-user directory.
- Payload logging is disabled by default.

### 7.5 Compatibility matrix

Maintain `tests/hardware-matrix.md`:

| Palm Desktop | Device | Palm OS | VID:PID | Windows build | Result | Notes |
|---|---|---|---|---|---|---|
| Reference | Reference | Reference | Recorded | Recorded | Required | Initial target |

### Exit criteria

- All core functional tests pass on the reference target.
- No known data corruption occurs.
- Lifecycle tests do not crash or permanently wedge HotSync Manager.
- Memory Integrity remains enabled throughout production acceptance testing.

---

## Phase 8 — Packaging, Signing, and Release

### Objectives

- Deliver a reversible installation.
- Protect existing Palm Desktop files and user data.
- Make diagnostics and rollback straightforward.

### Installer responsibilities

1. Detect Palm Desktop installation directory and version.
2. Hash `HOTSYNC.EXE` and the existing `USBPort.dll`.
3. Select only a known ABI profile.
4. Refuse unknown versions by default, with a documented diagnostic override.
5. Back up the original DLL without overwriting prior backups.
6. Install the x86 replacement DLL.
7. Add the signed WinUSB package to the driver store.
8. Associate only supported USB hardware IDs.
9. Preserve Palm user data and conduit configuration.
10. Offer a complete rollback that restores the original DLL and driver association.
11. Produce an installation report containing versions and hashes.

### Package layout

```text
PalmDesktopWinUSB/
├── setup.exe
├── bin/
│   └── x86/
│       └── USBPort.dll
├── driver/
│   ├── PalmWinUSB.inf
│   └── PalmWinUSB.cat
├── profiles/
│   └── supported-builds.json
├── tools/
│   ├── palm-usb-probe.exe
│   └── collect-diagnostics.exe
├── docs/
│   ├── installation.md
│   ├── rollback.md
│   ├── supported-devices.md
│   └── troubleshooting.md
└── licenses/
```

### Release channels

- **Research build:** proxy and verbose tracing; not for normal use.
- **Developer build:** WinUSB backend with local diagnostics.
- **Test build:** signed candidate for a fixed hardware matrix.
- **Stable build:** only validated DLL hashes and device IDs enabled.

### Exit criteria

- Clean installation on a fresh Windows 11 machine succeeds.
- Clean uninstall restores the original state.
- Production package works without test mode or disabling Memory Integrity.
- Unknown Palm Desktop builds are not modified automatically.

---

## 11. Repository Plan

```text
palm-desktop-winusb/
├── README.md
├── LICENSES.md
├── CMakeLists.txt
├── cmake/
│   └── toolchains/
│       └── windows-x86.cmake
├── docs/
│   ├── project-plan.md
│   ├── reference-target.md
│   ├── architecture.md
│   ├── original-driver-contract.md
│   ├── usbport-abi.md
│   ├── protocol-notes.md
│   ├── installation.md
│   ├── rollback.md
│   └── troubleshooting.md
├── evidence/
│   ├── README.md
│   ├── hashes/
│   ├── exports/
│   ├── traces/
│   └── registry/
├── abi/
│   ├── schema/
│   ├── manifests/
│   └── generated/
├── src/
│   ├── usbport/
│   ├── transport/
│   ├── protocol/
│   ├── diagnostics/
│   └── common/
├── proxy/
│   ├── proxy_loader.cpp
│   ├── proxy_trace.cpp
│   └── exports.def
├── tools/
│   ├── export-scanner/
│   ├── abi-diff/
│   ├── palm-usb-probe/
│   ├── trace-decoder/
│   └── diagnostic-collector/
├── driver/
│   ├── PalmWinUSB.inf
│   ├── hardware-ids.yaml
│   └── scripts/
├── tests/
│   ├── unit/
│   ├── abi/
│   ├── transport/
│   ├── protocol/
│   ├── replay/
│   ├── integration/
│   └── hardware-matrix.md
├── packaging/
│   ├── installer/
│   ├── supported-builds.json
│   └── rollback/
└── third_party/
    └── README.md
```

Do not commit proprietary Palm or Aceeca binaries to a public repository. Store hashes, metadata, derived ABI descriptions, and independently created test fixtures instead.

---

## 12. Build System

### Required outputs

- `USBPort.dll`: Windows x86 DLL.
- `palm-usb-probe.exe`: x86 or x64 command-line tool.
- Unit and replay tests.
- INF and catalog-generation inputs.
- Installer and rollback utility.

### Compiler configuration

- Visual Studio C++ toolchain.
- Windows SDK headers and libraries.
- CMake presets for x86 Debug, x86 Release, and tool builds.
- `/W4` or stricter warnings.
- Treat project warnings as errors after initial reverse-engineering scaffolding stabilizes.
- Enable Control Flow Guard where compatible.
- Enable ASLR and DEP/NX compatibility.
- Avoid static initialization that performs OS or USB operations.

### Continuous integration

CI can verify without physical Palm hardware:

- x86 DLL builds.
- exact export names and ordinals.
- PE architecture.
- manifest/schema validation.
- unit tests.
- captured-session parser tests.
- replay transport tests.
- formatting and static analysis.
- installer dry-run logic.

Physical hardware integration tests remain a separate controlled job.

---

## 13. Test Infrastructure

### 13.1 Transport abstraction

Define a narrow interface so the DLL can use real USB, recorded sessions, or a fault simulator:

```cpp
class ITransport {
public:
    virtual ~ITransport() = default;
    virtual Result wait_for_device(CancelToken cancel) = 0;
    virtual Result open() = 0;
    virtual Result read(std::span<std::byte> buffer,
                        std::size_t& transferred,
                        Deadline deadline) = 0;
    virtual Result write(std::span<const std::byte> buffer,
                         std::size_t& transferred,
                         Deadline deadline) = 0;
    virtual void cancel() noexcept = 0;
    virtual void close() noexcept = 0;
};
```

The exact code may differ, but the abstraction must support:

- asynchronous cancellation;
- partial transfers;
- timeouts;
- disconnect;
- deterministic replay.

### 13.2 Replay backend

Create a replay transport that consumes sanitized traces and validates expected writes. It should support fault injection:

- timeout after packet N;
- short read;
- short write;
- malformed packet;
- duplicate packet;
- disconnect;
- delayed arrival;
- cancellation race;
- callback failure.

### 13.3 ABI tests

Automate checks for:

- file architecture;
- export names;
- export ordinals;
- decorated symbols;
- function forwarding table completeness;
- selected structure sizes and offsets;
- known return-code mappings.

---

## 14. Diagnostics Strategy

### Log levels

- `ERROR`: operation failed and affects HotSync.
- `WARN`: recovered anomaly or unsupported behavior.
- `INFO`: session lifecycle and device identity.
- `DEBUG`: function calls and state transitions.
- `TRACE`: USB packet metadata and optional payloads.

### Required event fields

- timestamp
- process and thread ID
- build identifier
- Palm Desktop ABI profile
- device profile
- session ID
- state transition
- API or export name
- transfer direction and size
- result category
- Windows error
- USB status where available

### Privacy and safety

Palm synchronization can contain contacts, calendar entries, memos, and other personal data. Therefore:

- Do not log payload bytes by default.
- Redact user names and serial numbers in standard logs where practical.
- Make payload logging explicit and temporary.
- Mark diagnostic archives as potentially sensitive.
- Provide a sanitizer before traces are attached to bug reports.

---

## 15. Risk Register

| Risk | Impact | Mitigation |
|---|---|---|
| Unknown `USBPort.dll` ABI | Project blocker | Exact proxy first; dynamic tracing; per-hash ABI profiles |
| Wrong x86 calling convention | Stack corruption and crashes | Preserve decorated exports; assembly-aware wrappers; proxy comparison tests |
| Palm Desktop versions use different ABIs | Compatibility failures | Hash-gated installer and separate ABI manifests |
| Original DLL depends on undocumented kernel behavior | WinUSB backend mismatch | Correlate DLL calls with IOCTL and USB traces; retain UMDF fallback only if proven necessary |
| Different devices use different framing | Device-specific failures | Device profile layer; add one model at a time |
| Device appears only briefly after HotSync button | Missed sessions | Continuous notification/listen state; fast open; repeated-enumeration tests |
| Disconnect race crashes HotSync Manager | Poor reliability or data loss | Explicit state machine, cancellable I/O, idempotent close, replay fault tests |
| Driver package loses ranking to old package | Wrong driver loads | Installer removes or demotes obsolete package safely; verify active stack |
| INF/CAT signing blocks deployment | Release blocker | Treat signing as an early packaging workstream, not a final afterthought |
| Palm data corruption | Severe | Disposable profiles, backups, record-level verification, staged conduit tests |
| Proprietary binary redistribution | Legal/release risk | Do not ship original Palm/Aceeca binaries; distribute only original implementation and metadata |
| pilot-link licensing mismatch | Release risk | File-level license review before reuse; use as behavioral reference if uncertain |
| DLL hijacking or insecure loading | Security risk | Absolute paths, restricted install directory, safe DLL search behavior, signed releases |
| HotSync Manager hangs on blocking call | User-visible failure | Match original timing; worker thread; cancellation; bounded waits |

---

## 16. Decision Gates

### Gate 1 — Can the DLL be proxied transparently?

Proceed only after Palm Desktop successfully completes a HotSync through the exact proxy DLL.

If this fails, correct the ABI/export/loading assumptions before any WinUSB work is integrated into Palm Desktop.

### Gate 2 — Is WinUSB sufficient?

WinUSB is considered sufficient when the standalone probe can:

- open the device;
- discover endpoints;
- reproduce the initial handshake;
- transfer the required stream or packets;
- cancel cleanly;
- recover after disconnect.

Do not begin a UMDF/KMDF driver unless a specific, documented WinUSB limitation blocks a required operation.

### Gate 3 — Can one export path run natively?

At least one complete semantic operation must run through WinUSB while the remaining operations still forward to the original DLL.

### Gate 4 — Can the original DLL and SYS be removed?

Proceed to broad testing only after a full HotSync succeeds without loading either the original `USBPort.dll` or obsolete Palm/Aceeca `.sys`.

### Gate 5 — Is release installation reversible?

No stable release until install and rollback are tested on a clean Windows 11 environment.

---

## 17. Milestones and Acceptance Criteria

### M0 — Reproducible reference environment

- Known-good legacy HotSync captured.
- Exact binaries and IDs archived.
- Windows 11 security baseline recorded.

### M1 — ABI map

- Used exports and ordinals known.
- Initial function signatures and call sequence documented.
- ABI manifest created.

### M2 — Transparent proxy

- Proxy DLL loads.
- Full HotSync succeeds while forwarding.
- Calls are logged without behavioral regression.

### M3 — Driver and USB behavior map

- Original IOCTL/device-interface behavior documented.
- USB capture correlated with DLL call timeline.
- Device profile created.

### M4 — WinUSB transport proof

- Signed or controlled development WinUSB package installs.
- Probe opens device and completes handshake.
- Disconnect and reconnect work.

### M5 — Hybrid DLL

- At least one Palm Desktop operation uses the native WinUSB backend.
- Remaining operations can still forward for comparison.

### M6 — Native full HotSync

- Original DLL and SYS removed.
- Backup, synchronization, and PRC/PDB installation succeed.
- Repeated HotSync sessions succeed.

### M7 — Hardened reference release

- Lifecycle and data-integrity tests pass.
- Installer and rollback pass.
- Memory Integrity and Secure Boot remain enabled.

### M8 — Expanded device support

- New devices added through explicit device profiles and hardware tests.
- No regression on the reference target.

---

## 18. Initial Issue Backlog

### Research

- [ ] Hash and archive installed Palm Desktop binaries.
- [ ] Extract original DLL exports and ordinals.
- [ ] Determine whether HotSync uses static imports or `GetProcAddress`.
- [ ] Record original driver device-interface GUID.
- [ ] Record all USB hardware IDs from the original INF.
- [ ] Capture a successful USB HotSync.
- [ ] Build call/IOCTL/USB correlation timeline.
- [ ] Review pilot-link USB and DLP implementation and licenses.

### Proxy

- [ ] Generate exact `.def` file.
- [ ] Build x86 proxy DLL.
- [ ] Implement absolute-path original DLL loader.
- [ ] Add non-recursive forwarding.
- [ ] Add structured call tracing.
- [ ] Validate stack integrity and return values.

### WinUSB

- [ ] Generate project device-interface GUID.
- [ ] Write initial single-device INF.
- [ ] Implement SetupAPI enumeration.
- [ ] Implement `CreateFile` with overlapped I/O.
- [ ] Implement `WinUsb_Initialize` and cleanup.
- [ ] Enumerate endpoints dynamically.
- [ ] Implement asynchronous read/write/cancel.
- [ ] Implement device arrival/removal notification.
- [ ] Implement initial Palm handshake.

### Native DLL

- [ ] Define transport interface.
- [ ] Define session state machine.
- [ ] Centralize error mapping.
- [ ] Replace first export path natively.
- [ ] Add replay tests.
- [ ] Remove dependency on original DLL.
- [ ] Verify repeated HotSync sessions.

### Packaging

- [ ] Define supported-build manifest.
- [ ] Implement installer detection and hashing.
- [ ] Implement original DLL backup.
- [ ] Implement driver package install.
- [ ] Implement complete rollback.
- [ ] Prepare production signing path.
- [ ] Write troubleshooting collector.

---

## 19. First Implementation Sprint

The first implementation sprint should stop before writing the final WinUSB-backed DLL. Its purpose is to remove the largest uncertainty: the Palm Desktop ABI.

### Sprint tasks

1. Create the repository structure.
2. Archive and hash the exact original binaries.
3. Run `dumpbin` and PE inspection on `HOTSYNC.EXE` and `USBPort.dll`.
4. Create the first ABI manifest.
5. Generate an exact export `.def` file.
6. Build a minimal x86 DLL that logs load/unload.
7. Confirm Palm Desktop attempts to load it.
8. Build the forwarding proxy.
9. Complete a HotSync through the proxy.
10. Capture the full exported-call sequence.
11. Trace `CreateFile`, `DeviceIoControl`, `ReadFile`, and `WriteFile` calls.
12. Produce `docs/usbport-abi.md` and `docs/original-driver-contract.md`.

### Sprint completion condition

The sprint is complete only when the project can answer:

- Which DLL exports does HotSync Manager call?
- In what order?
- On which threads?
- With which known arguments and callbacks?
- Which operations open or control the original driver?
- Which behavior must be reproduced by the WinUSB backend?

---

## 20. Definition of Done

The project is complete for the initial target when:

1. Palm Desktop and HotSync Manager binaries are unmodified.
2. The replacement x86 `USBPort.dll` is loaded as the normal USB transport.
3. The Palm device uses Microsoft `winusb.sys`.
4. The obsolete Palm/Aceeca kernel driver is absent or inactive.
5. Windows 11 Memory Integrity and Secure Boot remain enabled.
6. A clean test profile completes backup and two-way synchronization.
7. PRC and PDB installation succeeds.
8. Cancellation, disconnect, reconnect, and repeated HotSync sessions work.
9. No crashes, hangs, or known data corruption occur in the acceptance matrix.
10. Installation and rollback are documented and tested.
11. The release package contains no proprietary Palm/Aceeca binaries.
12. The exact supported Palm Desktop builds and USB device IDs are documented.

---

## 21. Technical References

1. Microsoft, **WinUSB (Winusb.sys) Installation for Developers**  
   <https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/winusb-installation>

2. Microsoft, **Write a Windows Desktop App Based on the WinUSB Template**  
   <https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/how-to-write-a-windows-desktop-app-that-communicates-with-a-usb-device>

3. Microsoft, **WinUsb_Initialize function**  
   <https://learn.microsoft.com/en-us/windows/win32/api/winusb/nf-winusb-winusb_initialize>

4. Microsoft, **Implement Memory Integrity Compatible Code**  
   <https://learn.microsoft.com/en-us/windows-hardware/drivers/driversecurity/implement-hvci-compatible-code>

5. Microsoft, **Driver Compatibility with Memory Integrity and VBS**  
   <https://learn.microsoft.com/en-us/windows-hardware/test/hlk/testref/driver-compatibility-with-device-guard>

6. Microsoft, **PnPUtil Command Syntax**  
   <https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/pnputil-command-syntax>

7. pilot-link project, **Palm OS userland transport and synchronization toolkit**  
   <https://github.com/desrod/pilot-link>

---

## 22. Recommended Immediate Action

Begin with **Phase 0 through Phase 2 only**:

1. Preserve the exact original files.
2. Extract the ABI.
3. Build the exact forwarding proxy.
4. Prove a successful HotSync through that proxy.

That result creates a reliable compatibility harness. Only then should the project replace the obsolete driver backend with WinUSB one operation at a time.
