# TilkieTalkie — Factory First-Boot Bring-Up

Guide for flashing a board for the first time. The firmware **configures the battery gauge
itself** on first boot and **recovers a flat cell on its own** — no serial commands are
needed in normal operation. You mostly **watch the status LED**.

---

## 1. Set up the station

- **Cell installed.** Any state of charge is fine; it does **not** need to be full or empty.
  A brand-new cell at storage charge (~30–50 %) is ideal.
- **USB-C power connected** to the charger port (5 V / ≥ 2 A, or USB-C PD). This powers the
  board **and** supplies the charge current that first-boot calibration needs. **Leave it
  connected for the whole bring-up.**
- **Programmer / serial** connected (115200 baud) for flashing and for reading the boot log.
- **Room temperature** (10–40 °C).

> 🚫 Do **not** feed an external voltage into the SYS rail while USB is connected — power the
> board from the **USB-C charger only**. Double-feeding causes a charger fault (VSYS OVP).

---

## 2. Flash and watch the LED

Flash the production firmware. On first boot the device runs bring-up automatically.
**Watch the status LED — it tells you the phase:**

| LED | Meaning | Technician action |
|---|---|---|
| 🔵 **Blue** | Powering up / provisioning the gauge / charging a low cell | wait |
| 🟠 **Amber** | Waiting for charge current (calibration needs it) | **confirm USB-C power is connected** |
| 🩵 **Cyan** | Calibrating the current sense | wait (a few seconds) |
| 🟢 **Green** | ✅ **Bring-up complete — board PASSED** | proceed to §3 |
| 🔴 **Red** | ❌ Bring-up failed | see §5 troubleshooting |

Typical fresh-board sequence: **blue → (cyan) → green**, usually well under a minute on USB.
If a board's gauge was already configured + calibrated, it goes **blue → green** almost
instantly. If it sits on **amber**, it isn't seeing charge current → check USB power.

Bring-up runs **once per board** — a flag is saved in NVS. A re-flash does **not** re-run it.

---

## 3. Confirm and finish (on green)

In the serial terminal type **`battery`** and check:

- `Gauge Chem ID: 0x418`, `DA Configuration: 0x10 (1S)`
- `Gauge SafetyStatus: 0x0 (None)`
- `Full Charge Capacity` non-zero (~3500–4000 mAh)
- `Battery Current` ≈ the charge current (NOT several times higher) — confirms calibration
- `Charge State: Fast charge`, `Status: Normal`, no `Brownout` messages

Board passed → let it finish charging as needed. Done.

Optional: type **`gaugeprot`** and confirm `Protection Configuration (0x46AE): 0x02` (or
`0x03`) — i.e. **CUV latch enabled**. This is the over-discharge protection; it is set
automatically on every boot, but it's worth spot-checking on the first units of a run.

---

## 4. Deep-discharge recovery (shelf / forgotten units) — automatic

A LiFePO4 cell left for a long time can self-discharge until the gauge's protection
disconnects it. **Recovery is hands-free, but requires USB power:**

1. Connect **USB-C power**. (A disconnected/flat cell can only be revived by the charger —
   on battery alone the board stays off until plugged in. This is normal.)
2. The board powers up and shows **blue** ("charging / recovering"). The gauge may take up to
   ~15–30 s to wake from its low-power reset state — the firmware **waits patiently**; this is
   not a fault.
3. Once the cell rises past ~3.1 V, the device **starts the full system on its own**.

No serial commands, no re-flash. Leave it on the charger and it comes back.

---

## 5. Troubleshooting

| LED / symptom | Likely cause | Fix |
|---|---|---|
| 🟠 Amber, never advances | No charge current | Confirm USB-C power is live. The cell must be able to draw charge (not 100 % full). |
| 🔵 Blue for a long time on a very flat cell | Gauge still waking / cell still precharging | Wait — on USB it recovers. If it never advances after several minutes, the cell or gauge may be damaged → rework. |
| 🔴 Red (failed) | Gauge not detected, or calibration failed repeatedly | Check the cell connector / gauge soldering; verify USB power. Re-flash and retry. If it persists, set the board aside for rework. |
| Never powers up unless extra power is applied | SYS being fed alongside USB | Remove the extra SYS feed — **USB power only**. |

**To re-run bring-up** on a board (re-test / refurbish): in the serial terminal type
**`bringupreset`** — it clears the NVS flag and restarts the sequence.

---

## 6. Notes (engineering)

**What bring-up does, per board (first boot only):**
1. Provisions the gauge for LiFePO4 (1S, Chem ID 0x418, 4000 mAh, charge/current thresholds)
   — done non-destructively by `battery.begin()`.
2. Enables the **CUV latch** (Protection Configuration `0x46AE` bit 1) so an undervoltage trip
   stays latched until a charger is present — proper over-discharge protection. Re-applied on
   **every** boot (idempotent), so units flashed with this firmware get it even if their
   bring-up flag is already set.
3. **Calibrates the current sense** against the BQ25792's IBAT reference. This is per-board
   (depends on the sense resistor) and cannot be shared or skipped. Charge current is held at
   300 mA during bring-up and raised to 1 A once calibrated.

**Protection model (no manual intervention required):**
- *Prevent, while running:* firmware low-battery cutoff at 3.0 V → forces deep sleep.
- *Prevent, while off/asleep:* gauge **CUV latch** + GPIO4 held low in deep sleep (keeps the
  4.5 V peripheral rail off so the cell isn't drained in sleep).
- *Recover:* the device waits in a minimal "monitor" mode for a depleted cell to charge, then
  starts the full system automatically (see §4).

**Sequencing detail:** the heavy subsystems (SD, WiFi, BLE, audio, NFC) are deferred until the
cell can support them and bring-up is done. This prevents the brown-out/reboot loop a depleted
cell otherwise causes during WiFi init, and ensures the one gauge reset that calibration needs
happens at minimal load.

**Diagnostic serial commands (not part of the normal flow):**
- `battery` — full status dump.
- `gaugeprot` — read back the gauge protection config (CUV enable/latch/thresholds).
- `gaugecuvfix` — manually (re)apply the CUV latch and report whether it stuck.
- `bringupreset` — clear the bring-up flag and re-run provisioning/calibration.
- `startsystem` — force the full system to start (bench/debug, bypasses the battery-ready gate).
- `gaugeprog` / `gaugecalcurrent` — manual provision / calibrate (bring-up does these
  automatically; only needed for manual rework).

Never deep-discharge the cell during handling. The engineering `learndis` / `learnrest`
commands are **not** part of the factory flow.
