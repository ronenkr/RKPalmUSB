# Palm USB wire protocol — verified on hardware

Everything here was captured from a real Palm handheld (`0830:0040`, `PalmSN12345678`)
over WinUSB with `palm-usb-probe`, not taken from documentation. Bytes are exact.

## Transport

| | |
|---|---|
| Bulk IN | `0x82`, max packet 64 |
| Bulk OUT | `0x02`, max packet 64 |
| Vendor requests | **unsupported** — `GET_CONNECTION_INFO` (0x01) and `GET_EXT_CONNECTION_INFO` (0x04) both stall with error 31 |
| Endpoint selection | must come from the descriptor, preferring the **largest** max packet size — the 16-byte pair on endpoint 1 stalls on read |
| Availability | the device enumerates only while HotSync is active and drops off ~60 s later |

## NET framing

The handheld speaks Palm's NET protocol (pilot-link's `pi-net.c`), not SLP/PADP. There is
no `BE EF ED` magic anywhere in this conversation.

Every message is a 6-byte header followed by `length` payload bytes:

```
offset  size  field
0       1     type      always 0x01 for data
1       1     txid      transaction id
2       4     length    big-endian payload length
```

The header's length field predicts the next packet exactly, which is how the framing was
identified.

## NET handshake — the real one (pilot-link `libpisock/net.c`)

**The handshake is asymmetric, and the host's reply is 50 bytes, not a 22-byte echo.**

The handheld runs `net_tx_handshake()`, so the host must run `net_rx_handshake()`:

| step | device | host |
|---|---|---|
| 1 | TX 22 bytes `90 01 …` | RX |
| 2 | RX | **TX 50 bytes `12 01 …`** |
| 3 | TX 50 bytes `92 01 …` | RX |
| 4 | RX | **TX 46 bytes `13 01 …`** |
| 5 | TX 8 bytes `93 00 …` | RX |

Host message 1, 50 bytes:

```
12 01 00 00 00 00 00 00 00 20 00 00 00 24 ff ff ff ff 3c 00
3c 00 00 00 00 00 00 00 00 00 c0 a8 a5 1f 04 27 00 00 00 00
00 00 00 00 00 00 00 00 00 00
```

Host message 2, 46 bytes:

```
13 01 00 00 00 00 00 00 00 20 00 00 00 20 ff ff ff ff 00 3c
00 3c 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00 00 00 00
00 00 00 00 00 00
```

`c0 a8 a5 1f` is 192.168.165.31 and `04 27` is port 1063 — leftovers from NetSync's TCP
origins. pilot-link sends them unchanged over USB.

**Symptom of getting this wrong:** replying with a 22-byte echo of the device's greeting,
and never sending the 46-byte follow-up, leaves the link half-open. Every later DLP call
then fails — `0x12` with `error 4 (invalid parameter)` regardless of arguments, even with
`argc = 0`, which looks like an argument-encoding fault and is not.

## Earlier, incorrect handshake notes (kept as a record of the wrong turn)

Press the HotSync button; the device sends first.

**1. Device → host** (header `01 FF 00 00 00 16`, 22-byte payload)

```
90 01 00 00 00 00 00 00 00 20 00 00 00 08 00 00 01 00 00 00 00 00
```

**2. Host → device** — the same 22 bytes with the leading byte changed `0x90` → `0x12`:

```
01 FF 00 00 00 16                                    <- header
12 01 00 00 00 00 00 00 00 20 00 00 00 08 00 00 01 00 00 00 00 00
```

**3. Device → host** (header `01 FF 00 00 00 08`, 8-byte payload)

```
92 00 00 04 00 00 00 00
```

`0x92` is the acknowledgement. The link is now up and the device waits for the host to
drive DLP.

That reading was wrong twice over. The 8-byte `92 00 00 04` is the device reporting
**error 4 (invalid parameter)** for the echoed reply — not an acknowledgement. With the
correct 50-byte message 1 the device answers with a 50-byte `92 01 …` instead, and only
then does DLP work. See the section above.

## DLP layer — authoritative wire format

Taken from pilot-link source, not reconstructed:
[`libpisock/dlp.c`](https://github.com/desrod/pilot-link/blob/master/libpisock/dlp.c) and
[`include/pi-dlp.h`](https://github.com/desrod/pilot-link/blob/master/include/pi-dlp.h).

### Request

```
offset  size  field
0       1     command
1       1     argument count
2       ...   arguments
```

### Argument encoding

Chosen by length; the id is OR-ed with a flag:

```c
#define PI_DLP_ARG_FIRST_ID   0x20
#define PI_DLP_ARG_FLAG_TINY  0x00     /* len < 0xFF     */
#define PI_DLP_ARG_FLAG_SHORT 0x80     /* len < 0xFFFF   */
#define PI_DLP_ARG_FLAG_LONG  0x40
```

| form | bytes |
|---|---|
| tiny | `id\|0x00`, `len`, data |
| short | `id\|0x80`, `0x00`, `len` (2 bytes), data |
| long | `id\|0x40`, `0x00`, `len` (4 bytes), data |

Argument ids start at `0x20` and increment per argument.

### Response

```
offset  size  field
0       1     command | 0x80
1       1     argument count
2       2     error code, big-endian
4       ...   arguments
```

### Opcodes — and the mistake that cost several sessions

The `dlpFunctions` enum starts at `dlpReservedFunc = 0x0F`:

| function | opcode |
|---|---|
| `dlpFuncReadUserInfo` | `0x10` |
| `dlpFuncReadSysInfo` | **`0x12`** |

**`ReadSysInfo` is `0x12`, not `0x20`.** `0x20` is `PI_DLP_ARG_FIRST_ID`, the *argument*
id. Confusing the two produces `error 2 (illegal request)` for every attempt regardless of
how the argument is encoded, which looks like a framing problem and is not.

`dlp_ReadSysInfo` on the wire, with `PI_DLP_VERSION_MAJOR 1` / `PI_DLP_VERSION_MINOR 4`:

```
12 01 20 04 00 01 00 04
│  │  │  │  └─┬─┘ └─┬─┘
│  │  │  │    │     └── minor 4
│  │  │  │    └──────── major 1
│  │  │  └───────────── arg length 4
│  │  └──────────────── arg id 0x20 | TINY
│  └─────────────────── argc 1
└────────────────────── dlpFuncReadSysInfo
```

`ReadSysInfo` negotiates the version and must precede other calls, which is the likely
reason a correctly framed `ReadUserInfo` (`10 00`) was also refused.

## DLP layer (earlier notes)

After the handshake the host issues DLP (Desktop Link Protocol) requests inside NET
frames. A request payload is:

```
offset  size  field
0       1     command
1       1     argument count
2       ...   arguments
```

A response has the high bit set in byte 0 with the low bits echoing the request command
(so `0x10` → `0x90`), followed by an error code.

### Confirmed working, on hardware

With the correct handshake in place, a single HotSync button press now answers all of:

| request | response | result |
|---|---|---|
| `12 01 20 04 00 01 00 04` — `ReadSysInfo` | `92`, argc 2 | error 0, DLP version 1.2, Palm OS ~4.1 |
| `10 00` — `ReadUserInfo` | `90`, argc 1 | error 0, user name `m125` |
| `13 00` — `ReadSysDateTime` | `93`, argc 1 | error 0, device clock |

Opcodes `0x19`–`0x20` return error 6 (`none open`), which is correct: they operate on an
open database and none has been opened yet.

Decode a live device with:

```powershell
.\build\x64\palm-usb-probe.exe info     # then press HotSync once
```

which on the reference handheld prints:

```
--- system ---
  Palm OS            4.0.1 (0x04013001)
  locale             0x00010000
  product id         ""
  device DLP         1.2
  compat DLP         3.0
  max record         65505
--- user ---
  user id            7036
  user name          "m125"
--- clock ---
  device time        2000-01-01 22:38:06
```

`max record` is 0xFFE1, the ceiling on a single record transfer. The device reports an
empty product id (`prodIDLength` = 0), which is normal — not a decode fault. The clock
reads 2000-01-01 because this unit lost power; DLP timestamps come from the handheld, so
`last sync` tracks the device clock, not the PC's.

### Argument ids vs. argument positions

Response arguments carry ids starting at `PI_DLP_ARG_FIRST_ID` (0x20), but **dispatch on
position, not on id** — pilot-link's own accessor `DLP_RESPONSE_DATA(res, index, offset)`
takes an index. Matching a decoder against id `0x00`/`0x01` compiles, runs, reports
`error 0`, and prints nothing, which reads exactly like an empty device.

### Earlier note, superseded

Sending `10 00` (`ReadUserInfo`, no arguments) after the *incorrect* handshake produced:

```
TX  01 01 00 00 00 02 | 10 00
RX  01 01 00 00 00 08 | 90 00 00 02 00 00 00 00
```

`0x90` is `0x10 | 0x80`, argc `0`, error `0x0002`. The DLP layer was live and answering
even then; the rejection came from the half-open link, not from the command.

### Trap: `0x90` is ambiguous

`0x90` is *both* the NET handshake's opening message *and* the DLP response to command
`0x10`. Interpreting it by value alone makes a decoder answer a DLP response with a
handshake reply, get `0x92` back, and loop forever — which is exactly what the first
version of `palm-usb-probe netsync` did. **Track handshake-vs-DLP state explicitly.**

### DLP error codes

`0` none, `1` system, `2` illegal request, `3` memory, `4` parameter, `5` not found,
`6` none open, `7` already open, `8` too many open, `9` already exists, `10` cannot open,
`11` record deleted, `12` record busy, `13` unsupported, `15` read only.

Try a raw request of your own with:

```powershell
.\build\x64\palm-usb-probe.exe netsync                    # ReadSysInfo (the default)
.\build\x64\palm-usb-probe.exe netsync 1000               # ReadUserInfo
.\build\x64\palm-usb-probe.exe netsync 1201200400010004   # ReadSysInfo, spelled out
```

`netsync` loops, so one invocation serves many button presses. `sweep` and `info` each run
a single session, which is usually what you want — the handheld leaves the bus seconds
after the press and takes ~60 s to time out before it will accept another.

## Consequences for Palm Desktop — resolved

For a long time `USBTransport.dll` received these exact bytes through the named-pipe
bridge and never replied, which looked like a framing problem. It was not. HotSync was
never starting a session at all: `CUSBTransport*::PollConnection` gates on a private
`PalmUSBD.sys` IOCTL that `winusb.sys` does not implement, so it reported "no connection"
on every poll regardless of what arrived. See
[usbtransport-ioctls.md](usbtransport-ioctls.md).

Once that IOCTL is answered, HotSync drives this protocol correctly and a full sync
completes. **None of the framing above needed to change** — it was right, and
`palm-usb-probe` had already proven it end to end.

Worth keeping as a debugging lesson: "the peer reads our bytes and does not answer" was
consistent with a protocol fault *and* with the peer never having been told a connection
existed. Distinguishing them needed the peer's disassembly, not more protocol
experiments.
