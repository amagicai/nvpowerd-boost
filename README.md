# nvpowerd-boost

An `LD_PRELOAD` interposer for `nvidia-powerd` that lets you raise the
Dynamic Boost ceiling on NVIDIA laptops once you have patched your VBIOS to
allow a higher power limit.

`nvidia-powerd` asks the NVIDIA kernel driver for the platform power tables and
applies `base TGP + dynamic-boost delta` as the GPU power limit. This library
intercepts the RM ioctls that carry those values and adds your requested delta
on the way through.

Tested on: RTX 5090 laptop GPU, driver `615.78.08`, `nvidia-powerd 2.0`,
with a modified VBIOS loaded in memory (driver-visible `Max Power Limit`
raised to 200 W). Verified at `+15 W` over stock: 150 W base + 25 W stock
boost + 15 W delta = a **190 W** cap under load.

> **Safety first.** This changes how much power your GPU is allowed to draw.
> Raising it beyond what your VBIOS/VRM/cooling can sustain can damage the
> machine. The default delta is **0 (no change)** on purpose. Increase it in
> small steps and watch temperatures and stability. Use at your own risk.

---

## Requirements

This is **not** a standalone overclocking tool. It can only raise the limit up
to the maximum the driver already reports. Two things must be in place first.

### 1. A systemd system running `nvidia-powerd`

- Linux with **systemd**. This project installs a
  `nvidia-powerd.service.d` drop-in, so `nvidia-powerd` must be managed by
  systemd as a unit.
- The NVIDIA driver's `nvidia-powerd` daemon must be installed and working.
  Driver **615.x** with open kernel modules is what this was tested on;
  driver 535+ should use the same NVPCF RM interface, but only 615.78.08 has
  been verified.
- Check that the unit exists and is active:

  ```sh
  systemctl status nvidia-powerd
  systemctl cat nvidia-powerd
  ```

  If the unit does not exist, install your distribution's
  `nvidia-powerd`/driver package that provides it. `nvidia-smi` should list
  your GPU.

### 2. A patched VBIOS with headroom above the stock cap

The stock VBIOS/SBIOS power tables define the driver-visible **Max Power
Limit**. This library clamps every value to that maximum, so with an untouched
VBIOS there is nothing to unlock: the stock Dynamic Boost result is already the
ceiling.

You must load a modified VBIOS that raises the reported maximum. Use:

- **[nvidia-linux-vbios-vix-loader](https://github.com/Panchovix/nvidia-linux-vbios-vix-loader)**
  — loads a modified VBIOS per GPU at boot, in memory, through the open kernel
  modules. The flash chip is never written, and your normal boot entry remains
  as a fallback.
- **[NVIDIA VBIOS Vix Editor](https://github.com/Panchovix/nvidia-vbios-vix-editor)**
  — edits the ROM and produces the loader payload.

Follow that project's own README (it covers preparation, enrollment, Secure
Boot/signing and boot-entry setup). At a high level:

1. Dump your GPU's VBIOS and open it in Vix Editor.
2. Raise the power limit fields and save the edited ROM.
3. Normalize it into a payload (Ada/Ampere cards need the loader-specific
   `--representation` described in the loader README).
4. Install the loader, enroll the profile, and build/enable the boot entry.
5. Reboot into the loader entry.

After rebooting, confirm the new ceiling **before** installing this library:

```sh
nvidia-smi -q -d power | grep -A2 "Max Power Limit"
```

If `Max Power Limit` is still the stock value, the VBIOS is not being loaded
and this library cannot help. Keep a copy of the original ROM; the loader
README also documents how to boot the normal entry or disable the override if
something goes wrong.

### 3. Root

Installing the library, the systemd drop-in and restarting `nvidia-powerd`
all require root.

---

## Why this exists

Modern `nvidia-powerd` (driver 535+) has **no configuration file**. The old
`/etc/nvidia/nvidia-powerd.conf` with `boost=25` is gone. Instead the daemon:

1. asks the kernel driver for the platform power tables (NVPCF, sourced from
   the VBIOS / SBIOS ACPI),
2. reads a **base TGP** and a **dynamic-boost delta**,
3. applies `base + delta` as the GPU power limit under load.

So the stock `+25 W` boost lives in firmware/driver tables, not in userspace.
A patched VBIOS can raise the absolute maximum, but it cannot change how much
of that headroom `nvidia-powerd` chooses to use. This library bridges that
last gap by adding a userspace-controlled delta on top of the stock boost.

---

## Install

```sh
git clone <this-repository>
cd nvpowerd-boost
make
sudo make install
```

This installs:

| Path | Purpose |
|------|---------|
| `/usr/local/lib/nvpowerd-boost.so` | the interposer |
| `/etc/systemd/system/nvidia-powerd.service.d/nvpowerd-boost.conf` | systemd drop-in |

The drop-in is **inert by default** (`NVIOC_BOOST_DELTA=0`). Installing it does
not change your current power behaviour.

If you change `PREFIX`/`LIBDIR` when building, update the `LD_PRELOAD` path in
the drop-in to match where the `.so` was installed.

---

## Configure

Edit the drop-in:

```sh
sudoedit /etc/systemd/system/nvidia-powerd.service.d/nvpowerd-boost.conf
```

Set `NVIOC_BOOST_DELTA` to the number of **milliwatts to add** to the existing
boost:

| NVIOC_BOOST_DELTA | Meaning |
|-------------------|---------|
| `0` (default) | stock behaviour, no change |
| `10000` | +10 W beyond stock |
| `15000` | +15 W beyond stock |
| `25000` | +25 W beyond stock (stock 25 W boost becomes 50 W total) |
| `50000` | +50 W beyond stock |

> **It is an increment, not an absolute value.** The stock boost is already
> 25 000 mW, and the base TGP comes from the platform tables. The resulting
> limit is:
>
> ```
> effective limit = base TGP + 25 000 mW (stock boost) + NVIOC_BOOST_DELTA
> ```
>
> For example, with a 150 W base:
>
> | Delta | Effective cap |
> |-------|---------------|
> | `0` | 175 W (stock) |
> | `10000` | 185 W |
> | `15000` | 190 W |
> | `25000` | 200 W |
>
> Values are sanity-capped at 200 000 mW, and the final limit is always
> clamped to the driver-reported `Max Power Limit` (check with
> `nvidia-smi -q -d power`). A delta larger than the VBIOS headroom is
> silently clamped, not an error.

Then apply:

```sh
sudo systemctl daemon-reload
sudo systemctl restart nvidia-powerd
```

---

## Verify

Run a GPU workload, then check the cap:

```sh
nvidia-smi -q -d power | grep -A2 "GPU Ceiling Power Limit"
# or watch the live cap:
nvidia-smi --query-gpu=power.draw,power.limit --format=csv -l 1
```

A healthy result with a 150 W base and `NVIOC_BOOST_DELTA=15000` looks like:

```
187W / 190W
```

i.e. the GPU is allowed to draw up to 190 W. If the cap stays at the stock
175 W, see [Troubleshooting](#compatibility--troubleshooting).

---

## Dry run

To see what the library *would* do without changing anything, add:

```
Environment=NVIOC_PATCH=0
```

Then restart `nvidia-powerd` and read the log. The startup line and any
`[POWER]` lines are still written, and `(DRY-RUN)` is appended.

---

## Logs

By default the drop-in logs to:

```
/var/log/nvpowerd-boost.log
```

You get one line at startup and one each time the effective limit changes, for
example:

```
19:43:55.100 [BOOST] tier deltas 55000/45000/35000/25000 -> 70000/60000/50000/40000 mW (base=150000 max=200000 requested=+15000)
19:43:55.123 [POWER] limit 175000 -> 190000 mW (patched)
19:44:12.501 [POWER] limit 190000 -> 150000 mW
```

- `[BOOST]` is emitted once at startup when the boost-tier table is patched.
- `[POWER]` is emitted each time the applied GPU limit changes (boost on/off).

The header line also records the PID, configured delta, patch mode and log
path, which helps confirm the drop-in was actually loaded:

```
[nvpowerd-boost] pid=1234 delta=15000mW patch=1 log=/var/log/nvpowerd-boost.log
```

Read it with:

```sh
sudo tail -f /var/log/nvpowerd-boost.log
```

Set `NVIOC_LOG` in the drop-in to log somewhere else.

---

## How it works (exact mechanics)

`nvidia-powerd` talks to the driver through `/dev/nvidiactl` using the
NV-CONTROL RM-control escape (`_IOWR('F', 0x2a, 32)`, request `0xc020462a`).
The argument is an `NVOS54`-style struct:

```
offset  size  field
  0       4   hClient
  4       4   hSubDevice
  8       4   RM command id
 12       4   flags
 16       8   params pointer
 24       4   params size
 28       4   params size out
```

The interposer hooks `ioctl(2)`, decodes that wrapper, and acts on two
commands.

### 1. `0x2080a618` — `NVPCF_GET_ABS_POWER_LIMITS`

Called once at daemon start (POST direction, driver → daemon). In the 20 KiB
response on this platform:

- dword `185` = max power limit (mW) — e.g. `200000`
- dword `226` = base power limit (mW) — e.g. `150000`
- a descending boost-tier quartet somewhere near the tail:

```
[5077] = 55000
[5079] = 45000
[5081] = 35000
[5083] = 25000   <- the active 25 W dynamic-boost delta
```

The library finds that quartet by pattern (four power-like dwords, strictly
descending, spaced 30 000 mW apart, last ≤ 100 000), learns `base`/`max` from
the header, and adds `NVIOC_BOOST_DELTA` to **each** tier. The daemon then
computes `base + (25000 + delta)` on its own.

The learned `base` and `max` are also cached for the fallback below. If the
pattern or the header offsets do not match, the startup `[BOOST]` line is not
emitted and no patching happens.

### 2. `0x2080e634` — set power limit

Called when the daemon applies a limit (PRE direction, daemon → driver).
Dword `11` of the request carries the target limit in mW. The library:

- raises `base + 25000` (the stock boosted setpoint) to
  `base + 25000 + delta`, and
- clamps any value above the learned `max`.

This is a belt-and-braces second patch in case the daemon uses a cached value
instead of recomputing from the patched table. A value that already includes
the delta (`base + 25000 + delta`) does not match the stock setpoint, so it is
not boosted twice.

Both operations are no-ops when `NVIOC_BOOST_DELTA=0`, so installing the
library alone is safe.

---

## Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `NVIOC_BOOST_DELTA` | `0` | mW added to the boost tiers and setpoint |
| `NVIOC_PATCH` | `1` | `0` = log-only dry run |
| `NVIOC_LOG` | `/tmp/nvpowerd_ioctl.log` (library) / `/var/log/nvpowerd-boost.log` (drop-in) | log path |

---

## Build requirements

- Linux with a C compiler (`gcc`/`clang`) and `make`
- `libdl`, `pthread` (standard glibc)
- The runtime requirements above: **systemd + `nvidia-powerd`** and a
  **patched VBIOS** loaded through the Vix loader

---

## Uninstall

```sh
sudo make uninstall
sudo systemctl daemon-reload
sudo systemctl restart nvidia-powerd
```

Uninstalling this library does **not** remove the VBIOS loader or restore the
stock VBIOS. Follow the loader project's README (`nvbios system disable`, or
the normal boot entry) if you also want to undo that separately.

---

## Limitations

- **Applies to every NVIDIA GPU in the system.** There is no per-GPU filtering
  or selection: the delta is applied to whatever `nvidia-powerd` sees, so on
  multi-GPU machines all cards are affected.
- **Requires systemd and the `nvidia-powerd` service.** The provided
  installation mechanism is a systemd drop-in. Running the daemon another way
  requires wrapping it manually with `LD_PRELOAD`.
- **Requires a patched VBIOS.** The limit can never exceed the driver-reported
  `Max Power Limit`; without VBIOS headroom the delta is clamped away.
- **Only affects `nvidia-powerd`.** It does not change clocks, voltages,
  other power-management daemons, or the VBIOS itself.
- **Driver-layout dependent.** The dword offsets (`185`, `226`, `11`) and the
  tier pattern are specific to the driver-615 layout observed on the test
  platform; see [Troubleshooting](#compatibility--troubleshooting).
- **No persistence of its own.** Settings live in the systemd drop-in; they are
  re-applied whenever `nvidia-powerd` restarts. There is no CLI or config file.
- **Sanity caps.** Delta is capped at 200 000 mW, and only power-like values
  (multiples of 1000 mW in the expected range) are patched.

---

## Compatibility / troubleshooting

- Check the log first. If the `[BOOST]` line is missing, the response layout
  does not match the expected pattern and no patching occurs. The offsets may
  need revisiting for your driver version.
- If `(DRY-RUN)` appears, `NVIOC_PATCH=0` is set.
- If the limit does not rise:
  1. Confirm the driver-visible `Max Power Limit` is higher than the stock
     boost result (`nvidia-smi -q -d power`). If not, the patched VBIOS is not
     loaded — fix that first.
  2. Confirm the drop-in was actually loaded: look for the header line with the
     expected `delta=` value, and check
     `systemctl show nvidia-powerd -p Environment`.
  3. Confirm `nvidia-powerd` is the process being wrapped. The drop-in scopes
     `LD_PRELOAD` to that unit only.
- If the log shows a patched limit but `nvidia-smi` does not, remember that the
  daemon applies the boost only under load; the cap returns to base when idle.
- If a driver update changes the ioctl layout, use `tools/nv_ioctl_log.c` (see
  below) to rediscover the offsets.

---

## Development notes

`tools/nv_ioctl_log.c` is the raw traffic logger used to discover the ioctl
layout and the relevant dwords. It dumps the NVPCF payloads and flags
power-like values without modifying anything:

```sh
gcc -shared -fPIC -O2 -o nv_ioctl_log.so tools/nv_ioctl_log.c -ldl -lpthread
sudo systemctl stop nvidia-powerd
sudo LD_PRELOAD=$PWD/nv_ioctl_log.so NVIOC_LOG=/tmp/nv.log /usr/bin/nvidia-powerd
```

Stop the systemd unit first; otherwise it is already running and holds the
driver handles this logger wants. Start it again with
`sudo systemctl start nvidia-powerd` when done.

Use it as a reference when adapting the offsets or the tier pattern for a new
driver version.

---

## License

MIT
