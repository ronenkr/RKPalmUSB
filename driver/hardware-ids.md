# Palm OS handheld hardware IDs

Harvested from the legacy `AceecaUSBDx64.inf` (Aceeca International, 2011) in
[`oldusbdriver/`](../oldusbdriver/). This is the full list the obsolete driver claimed.

**None of these are in `PalmWinUSB.inf` by default except a few Palm-branded PIDs.** Add
one only after testing it on real hardware — an untested ID in a shipped INF means Windows
binds a device to `winusb.sys` that may then work worse than before.

Get the exact ID for your device with `.\scripts\Find-PalmDevice.ps1`.

## Palm — VID_0830

Base PIDs. The legacy INF also reserved `PID_00x1`–`00x4` variants around each base
(e.g. `0061`, `0062`, `0063`, `0064` alongside `0060`).

| PID | Notes |
|---|---|
| `0001`, `0002` | earliest USB models (m100 series era) |
| `0010`–`0014` | |
| `0020`–`0024` | |
| `0030`–`0034` | |
| `0040`–`0044` | |
| `0050`–`0054` | |
| `0060`–`0064` | Tungsten / Zire era — most likely for a modern test |
| `0070`–`0072` | |
| `0080`–`0082` | |

Reserved-range PIDs from the legacy INF: `0003`–`0006`, `0011`–`0014`, `0021`–`0024`,
`0031`–`0034`, `0041`–`0044`, `0051`–`0054`, `0061`–`0064`, `0071`–`0072`, `0081`–`0082`.

## Access / PalmSource — VID_13E8

`0600`, `FF00`

## Handspring — VID_082D

Visor and Treo: `0100`, `0200`, `0300`, `0400`, `0500`, `0600`

## Sony CLIÉ — VID_054C

`0038`, `0066`, `0095`, `009A`, `00DA`, `00E9`, `0144`

## Others

| Vendor | VID | PIDs |
|---|---|---|
| Kyocera | `0C88` | `0021` |
| AlphaSmart | `081E` | `DF00` |
| Acer | `0502` | `0001`, `0736` |
| Legend | `0E7C` | `2801` |
| Samsung | `04E8` | `661E`, `6620`, `6622`, `6624`, `8001` |
| Garmin | `091E` | `0004` |
| GSPDA | `115E` | `F100` |
| Fossil Wrist PDA | `0E67` | `0002` |
| Aceeca Meazura | `4766` | `0001` |

## Adding a device

1. `.\scripts\Find-PalmDevice.ps1` with the device idle — note the ID.
2. Press HotSync and run it again — **the PID often changes in HotSync mode**. Note that
   ID too.
3. Add both to `[Palm_WinUSB.NTamd64]` in [`PalmWinUSB.inf`](PalmWinUSB.inf):

   ```inf
   %PalmGeneric.DeviceDesc% = WinUSB_Install, USB\VID_0830&PID_XXXX
   ```

4. Re-sign the catalog, `.\scripts\Install-Driver.ps1`, then verify with
   `palm-usb-probe descriptors` and `palm-usb-probe listen`.
5. Record the result — model, Palm OS version, VID:PID, endpoints — so the next person
   does not repeat the work.

## Enumeration behaviour — read this before debugging

**A Palm handheld does not stay on the USB bus.** Sitting in the cradle it is invisible to
Windows; it enumerates only when you press the HotSync button, and drops off again after a
few seconds if nothing talks to it.

Consequences:

- `Find-PalmDevice.ps1` showing nothing does **not** mean the cable or driver is broken.
  Press the button, then run it.
- Any tool must be started *before* the button press. `palm-usb-probe` waits for arrival
  (`Waiting for the device. Press the HotSync button now+++`) for exactly this reason.
- Driver installation has to happen while the device is enumerated, so press the button
  first if `pnputil` cannot find it.
- `ERROR_GEN_FAILURE` (31) on a read is ambiguous: it means either a stalled endpoint or
  the device left the bus. Do not read it as "wrong endpoint" on its own.

## Tested

| Model | VID:PID | Bulk endpoints | Sync pair | Result |
|---|---|---|---|---|
| Palm Handheld (`PalmSN12345678`, USB 1.1, `bcdDevice 0100`) | `0830:0040` | `0x81`/`0x01` maxpacket 16, `0x82`/`0x02` maxpacket 64 | **`0x82` / `0x02`** | binds to `winusb.sys`; data confirmed flowing |

Notes on `0830:0040`:

- Reports `Palm, Inc.` / `Palm Handheld` / `PalmSN12345678`.
- **Exposes two bulk pairs, and only the 64-byte pair works.** Reading `0x81` fails with
  `ERROR_GEN_FAILURE`; `0x82` delivers data on the HotSync button press.
- **Supports neither vendor request.** `GET_CONNECTION_INFO` (0x01) and
  `GET_EXT_CONNECTION_INFO` (0x04) both fail with error 31. Endpoint selection therefore
  falls entirely to the descriptor heuristic — which is why that heuristic must prefer the
  largest max packet size rather than the first pair found.
- Observed on the button press, on `0x82`:

  ```
  6 bytes   01 FF 00 00 00 16
  22 bytes  90 01 00 00 00 00 00 00 00 20 00 00 00 08 00 00 01 00 00 00 00 00
  ```

  Note this is **not** `BE EF ED` SLP framing. The leading `90 01` is consistent with
  Palm's NetSync-style framing, which `USBTransport.dll` handles in its
  `CUSBTransportHTAL` class rather than the SLP-based `CUSBTransportPAD` class. Both
  transports sit *above* `USBPort.dll`, so the DLL needs no change — it is a byte pipe and
  `USBTransport.dll` picks the transport. The `BE EF ED` check in `palm-usb-probe` is a
  convenience hint for SLP devices, not a requirement.
