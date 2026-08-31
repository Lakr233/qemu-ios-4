# iPhone 3G QEMU cloud handoff

Updated: 2026-08-31 (GMT)

## 2026-08-31 playable-path and startup diagnosis

The ordinary `make -C build iphone3g-play` path now keeps the live-proven
software MBX backend.  It still opens resizable Cocoa with a visible Host
cursor and interpolated scaling.  Metal remains available only through the
explicit `IPHONE3G_MBX_METAL=1` experiment; forcing it in the stable target
produced a repeatable black presentation while the software path republished
the complete lock screen on the same persistent VM.

A clean software-MBX cold boot has reached a complete SpringBoard home screen
with status bar, icons, and Dock.  A later controlled single-thread run spent
more than 40 Host minutes before its first MBX submission, even though the
12 MHz RTC advanced at 12,282,204 ticks per Host second (about 1.02x wall
time).  Timer deadlines were normally about 8 ms of Guest time while the work
between successive rearms consumed roughly 90 ms of Host time.  The long
first-screen interval is therefore translated ARM1176 instruction throughput
plus Guest startup work, not a slow emulated clock, SpringBoard watchdog cycle,
or sleeping display backend.

A later instrumented single-thread run separated those costs more precisely.
Legacy FTL recovery and launchd completed after about 2 minutes 35 seconds;
the first accepted MBX commands did not arrive until 15 minutes 24 seconds on
the dirty-HFS boot.  The accepted sequence included complete 614,400-byte 2D
surfaces and the lock-screen sprite/status submissions with no decoder reject.
`.artifacts/runs/iphone3g/screen.wake.png` is the corresponding live lock-screen
witness.  During its later black interval the Guest had explicitly written
`LCD_DISABLE=1` and cleared `VIDCON0` enable, so the retained pixels and Cocoa
window were not the failure.  The installed Guest's own Auto-Lock setting was
one minute, which explains the repeatable visible-then-black transition.

The QMP Zephyr2 path was also closed live.  A paced 12-sample swipe arrived
with the controller powered and its downloaded firmware running; the Guest
consumed every start, move, and release frame with at most two queued.  The
controller's eight entries are a backlog, not a whole-gesture limit, so the
temporary seven-position cap was removed.  A gesture sent before the first
visible frame still unlocked the eventual SpringBoard presentation.

The clean-shutdown helper now delegates pairing and session establishment to
pymobiledevice3's canonical `autopair=True` state machine, submits Diagnostics
Sleep, and enables only focused ADM/FMC commit events.  A live run wrote all
child metadata followed by a fresh type-`0x43` clean FTL root, then stopped the
authenticated QEMU owner before the unmodeled deep-sleep path.  Its next cold
boot reached `FTL_Open [OK]`, mounted both filesystems, and started launchd
without `_FTLRestore` or `Recovering NAND`.  Diagnostics Shutdown by itself had
reached `reboot(RB_HALT)` and USB PHY power-down without this FTL commit.

An MTTCG experiment cut the initial FTL/launchd interval substantially, but it
also exposed a real cross-thread interrupt race: an I2C producer could clear
its owner state while VIC0 retained IRQ 21.  Both daisy-chained PL192
controllers now serialize their complete input/priority/CPU-output state under
one shared chain lock, and the stale ownerless IRQ no longer reproduced.
MTTCG nevertheless failed to complete a repeatable AppleUSBMux/SpringBoard
boundary after wake testing, so it remains the explicit
`iphone3g-play-fast` experiment.  The ordinary `iphone3g-play` target stays on
the slower, live-proven single-threaded TCG path.

A later MTTCG resume from a single-thread mid-boot checkpoint superseded that
negative experiment: it reached complete MBX2D/MBX3D submissions and a full
lock screen in about 32 Host seconds, with no decoder rejection.  MTTCG remains
opt-in for cold boots, but it is now the verified fast execution mode for the
disk-bound ready checkpoint described below.

The development harness now has `iphone3g-checkpoint` and `iphone3g-resume`.
The checkpoint binds VM state to immutable copy-on-write clones of NAND, NOR,
GID, and UID material and hashes every file; each resume uses a disposable
clone.  Snapshot creation resumes the warm source VM after publishing, so a
failed candidate cannot discard the playable session.  The first candidate
exposed an ARM32-with-EL3 migration bug: converting the KVM-shaped CP-register
indexes back to QEMU keys forced Secure AArch32 entries into the Non-secure
bank.  Its saved `DACR_S` and `TTBR1_S` were therefore zero, and resume
recursively entered the high data-abort vector.  VM-state conversion now
preserves the serialized secure-state bit while the real KVM synchronization
path retains its original Non-secure behavior.  An atomic checkpoint repaired
from the same stopped source resumed under the fixed binary and advanced
repeatedly through SVC and IRQ PCs instead of either abort or
undefined-instruction handler.

The release checkpoint now freezes the source before outgoing migration, so
the migration stream captures the exact authenticated visible frame instead
of allowing Auto-Lock to win during pre-copy switchover.  Its first Cocoa
restore reproduced the complete 320 by 480 lock screen, and the final
`iphone3g-resume` target automatically continued the incoming paused VM.  The
resumed Guest consumed complete Zephyr2 touch records, and a live Power-button
probe changed GPIO pad 22 bit 5 and caused AppleM68Buttons to re-arm its group-1
polarity from `0x1900` to `0x3900`.  Input migration is therefore closed
separately from the Guest's later valid `LCD_DISABLE`/Auto-Lock black frame.
The monitor, screenshot, wake, swipe, and stop targets now discover the active
quick-resume directory rather than silently addressing the cold-boot VM.
A final clean resume followed by an immediate 12-frame Zephyr2 swipe advanced
through the status-bar-only transition and published the complete SpringBoard
icon grid and Dock.  `.artifacts/runs/iphone3g-quick/springboard-proof.png` is
the 320 by 480 witness from the VM left running for interactive use.

## 2026-08-30 local Metal and display-timing update

An Apple M4 Max host accepted the opt-in Metal backend and completed its
realization self-test.  A cold installed-system boot then completed eight
live Guest source-over submissions on `Apple M4 Max`, with zero
`s5l8900_mbx_metal_failure`, `s5l8900_mbx_3d_reject`, or
`s5l8900_mbx_2d_reject` events.  This is the first live Metal Guest witness;
it remains a narrow acceleration of the measured source-over operation, not a
general PowerVR-to-Metal translation.

The same run published a complete 320 by 480 lock screen after 269 Host
seconds.  It contained 13,967 colors.  Eleven seconds later the Guest removed
the active LCD window, wrote `LCD_DISABLE=1`, and cleared `VIDCON0` bit 0.
A modeled Home press republished a 14,116-color lock screen before the normal
short lock-screen timeout blanked it again.  There were no GPU rejects across
either transition.  A five-second Host stack sample during the later black
interval was dominated by normal single-core TCG Guest execution and routine
timer/I2C/VIC exits, not Metal, LCD composition, or NAND I/O.

The user independently reached the activated Settings application and
interacted with it after allowing the slow Guest path to run for roughly ten
minutes.  Therefore a black interval alone is no longer evidence of a failed
display or GPU.  The remaining usability work is to shorten the single-core
TCG/legacy-FTL startup and post-unlock latency and to make wake/unlock timing
convenient without overriding the Guest's valid LCD power writes.

`make -C build iphone3g-qemu-play` selects resizable Cocoa, a visible Host
cursor, interpolation, and the software MBX backend by default.  With that QEMU
process running, `make -C build iphone3g-wake` sends a QMP-native Home button
press and `make -C build iphone3g-swipe-unlock` sends the existing Zephyr2
gesture.

## 2026-08-29 persistent activation and GPU update

The current persistent 8C148 development VM completed the official restore,
is cold-bootable under pure single-core TCG, is persistently hacktivated, and
has a live-proven software MBX-to-LCD path. This supersedes the older
in-progress boundaries retained later in this file.

- Official `iPhone1,2_4.2.1_8C148_Restore.ipsw`, SHA-256
  `98e5969c3baed660c9a26e94cd7ed4b3cdb7175900f448bcc2223bf885835ce0`.
- ASR completed 2,303/2,303 chunks and exactly 632,025,600 bytes; the Guest
  verified the image, passed root and data `fsck_hfs`, and installed the
  kernelcache.
- Catalog LLB and all eleven AP NOR transactions reached their real provider;
  the native no-baseband and absent-IR-MCU paths completed; restored returned
  terminal `Status: 0`.
- Every authenticated temporary Guest instruction was restored and read back
  before its owner detached. The focused ADM/FMC and AES failure trace was
  empty.
- A clean-context normal-watchdog boot reaches `FTL_Open [OK]`,
  `BSD root: disk0s1`, and launchd without `_FTLRestore` or `Recovering NAND`.
  AppleMobileDevice and USB Ethernet publish,
  and usbmux reports build `8C148`, version `4.2.1`, and the persistent device
  UDID `883c57dcfdac92d90e851dea7c1fd5888a4b1ae6`.
- `make -C build iphone3g-install-activation` used the authenticated Apple
  8C148 `restored_external`, its signed `fsck_hfs`/`mount_hfs` utilities, and
  bounded Guest libc calls to install and byte-verify the 286-byte
  `/mnt2/root/Library/Lockdown/data_ark.plist`. A later cold boot reached
  paired lockdownd with `ActivationState = FactoryActivated`; the change is
  persistent in the development NAND and is not a transient boot patch.
- The software MBX implementation is live-proven with real Guest MBX2D and
  MBX3D commands, GART translation, and physical LCD double buffers at
  `0x0f496000` and `0x0f52c000`. A visible 320x480 iOS 4 lock screen was
  captured at `.artifacts/runs/iphone3g/screen.gpu-first3d-persistent-20260829.png`
  with SHA-256
  `91b93c7f850c9e4c9ff8253dea2c509a0d081f71de3d9a3a234b18d6072e4454`.
  The trace contains no decoder rejection. Later black frames follow the
  Guest's explicit `LCD_DISABLE=1` and `VIDCON0=0x440`, not a failed MBX
  render or address translation.
- Older dirty images enter the Legacy FTL no-root recovery branch after a
  runtime type-`0x4f` marker.  Diagnostics Sleep now closes that state with a
  complete child-page chain and type-`0x43` root, and a subsequent cold boot
  has verified direct context loading.
- Cold UI boots currently use the authenticated `--hide-unmodeled-baseband`
  DeviceTree compatibility option so iOS does not enter its `Restore Needed`
  path. This is separate from activation and does not claim a modeled modem or
  cellular service.
- SpringBoard is alive, reports the device as `FactoryActivated`, receives the
  complete 12-sample Zephyr2 gesture, and has published a stable post-unlock
  home screen through the software MBX path.
- The optional Metal forwarding path remains experimental. The current cloud
  Mac exposes no usable Metal device, so only the software MBX path has a live
  execution witness.
- Apple USB Ethernet publishes and the PPP/libslirp implementation passes its
  protocol tests, but no live bidirectional Guest IP witness has completed.
  End-to-end networking remains open.

The release regression gate on this tree is the PPP protocol test, 85 qtest
subtests, and 113 Python tests; all passed after the lifecycle, input, display,
and MTTCG interrupt changes.
The persistent files are under `.artifacts/runs/iphone3g/`; keep
`nand.raw`, `nor.raw`, `gid-key.bin`, and `uid-key.bin` together. Ordinary
prepare and boot targets preserve them; `iphone3g-reset-nand` is the explicit
destructive reset boundary.

## Repository state

This work is based on current upstream QEMU, not the old `qemu-ios` fork.
The working machine is `iphone3g` (`iPhone1,2`, `N82AP`, `S5L8900`) and is
deliberately fixed to one ARM1176JZF-S CPU, 128 MiB RAM, pure TCG with JIT
translation, and a 320 by 480 panel.

- Remote: `https://github.com/Lakr233/qemu-ios-4.git`
- Branch: `master`
- Latest implementation checkpoint before this document update:
  `3f77099` (`iphone3g: persist activation and trace GPU scanout`)
- QEMU version: `11.1.50`
- Last local host: macOS 27.0 build 26A5421a, Xcode 27 beta 6, Apple clang 21
- Latest `make -C build iphone3g-test`: 85 qtest subtests and 113 Python tests
  passed; the separate PPP protocol test passed as well

The top-level `.gitignore` excludes `.artifacts`, `.venv`, `build`, and the
configure-generated `GNUmakefile`. Firmware, fuse material, NAND/NOR images,
logs, and binaries must remain outside Git.

## Goal and current boundary

The final goal remains an upstream-maintainable iPhone 3G machine that boots
official iOS 4.2.1 (8C148) under pure TCG, reaches an activated SpringBoard,
renders through an emulated GPU, and has usable networking with live evidence.

The boot/restore boundary, persistent hacktivation, software GPU execution,
stable post-unlock SpringBoard presentation, and modeled input are met. The
overall upstream machine goal remains open until a bidirectional Guest network
witness is recorded.
Metal acceleration is an optional optimization and may only be claimed after
running on a Host with a usable Metal device. Baseband/cellular service, Wi-Fi,
direct FTL context reload, and full audio remain explicit hardware gaps. The
older 5A347 and intermediate 8C148 sections below are retained as diagnostic
history and must not be read as the current state.

## Current continuation commands

The retained VM identity is in `.artifacts/runs/iphone3g/`. Do not replace its
NAND, NOR, GID key, or UID key individually.

```sh
make -C build iphone3g-qemu
make -C build iphone3g-install-activation
make -C build iphone3g-boot \
  IPHONE3G_BOOT_ARGS='rd=disk0s1 -v serial=3' \
  IPHONE3G_BOOT_PROFILE_ARGS='--hide-unmodeled-baseband'
make -C build iphone3g-swipe-unlock
make -C build iphone3g-test
```

The activation install is a one-time operation for a restored NAND; ordinary
cold boots consume the persisted `data_ark.plist`. Use
`iphone3g-boot-installed-hacktivated` only as a transient diagnostic fallback.
Set `IPHONE3G_MBX_METAL=1` only on a Metal-capable Host and retain the software
trace/screenshot as the correctness baseline.

## External artifacts that must move separately

The fastest continuation is to transfer these ignored directories without
changing any bytes:

1. `.artifacts/runs/5A347-terminal-20260828/` (8.4 GiB)
2. `.artifacts/firmware/5A347/` (1.8 GiB)
3. `.artifacts/firmware/8C148/` (8.5 GiB, mainly the exact erased NAND base)
4. `.artifacts/firmware/iPhone1,2_4.2.1_8C148_Restore.ipsw`
5. `/Users/qaq/Desktop/iPhone1,2_2.0_5A347_Restore.ipsw`

Use a sparse-preserving transport such as `rsync -a --sparse`. Do not transfer
`build`, `.venv`, `GNUmakefile`, monitor sockets, or Python cache directories;
they are host-local and reproducible.

The run directory's `gid-key.bin` and `uid-key.bin` must stay with its NAND and
NOR. The matching `.artifacts/firmware/5A347/gid-kbags.bin` must also move.
Generating replacement fuse keys makes the saved VM a different device and
breaks the encrypted KBAG relationship.

Authoritative hashes after the clean QEMU shutdown:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| 5A347 IPSW | 235,957,125 | `76fd34606cda0e2943766878c2cad4e9ee38e15084094240fba68e4391cba8f1` |
| `5A347-terminal-20260828/nand.raw` | 9,042,919,424 | `1d5a6a15705977bf1ead9f8b396853d0565ba9d4d00cc183db63ed714689f4a9` |
| `5A347-terminal-20260828/nor.raw` | 1,048,576 | `a8b430791ef307d25fe7ef65d3a6ff36a6a7f94d3151657ef459e8071a5dc3c7` |
| `5A347-terminal-20260828/uart0.log` | 388,059 | `d5928f870bb5d23c69802d762e98154ac6216bbd4fcae29d8536f9bac3f0436d` |
| `5A347-terminal-20260828/screen-final.ppm` | 460,815 | `fd57e5de91d9694d8590c88c7383220f44ff3ad5d0260fe71e8e1cc9ebf7628c` |
| 8C148 IPSW | — | `98e5969c3baed660c9a26e94cd7ed4b3cdb7175900f448bcc2223bf885835ce0` |

`screen-final.ppm` is a real 320 by 480 RGB screendump. The focused QEMU trace
for the run is zero bytes, so neither the ADM/FMC failure tracepoint nor the
AES failure tracepoint fired.

## Latest 5A347 restore evidence

The fresh run used:

- `-M iphone3g,security-epoch=4`
- `-accel tcg,thread=single`
- `-smp 1`
- Cocoa display
- the repository's pinned `.venv` and `usbmuxd-usboip` bridge
- official `iPhone1,2_2.0_5A347_Restore.ipsw`

The live restored service authenticated as `N82AP`, protocol 11. `StartRestore`
was sent exactly once after `FTL_Open [OK]`. The ASR progress bar reached
1568/1568, copied 458,469,888 bytes, verified the complete block checksum, and
successfully updated both HFS volume headers. The authenticated temporary ASR
and partition-wipe patches were read back and restored before their owners
were released.

The Guest then produced the strongest available storage evidence:

```text
** The volume BigBear5A347.N82OS appears to be OK.
** The volume Data appears to be OK.
entering fixup_var
entering unmount_filesystems
entering update_NOR
```

The only terminal failure was:

```text
img3_flash_NOR_image: flashing LLB data
img3_flash_NOR_image: flashing NOR data
IOConnectCallStructMethod(1) failed: 0xe00002c2
restore completed (-1), requesting reboot
```

The Host received restored `Status=-536870206`, which is the same
`0xe00002c2` (`kIOReturnBadArgument`). The authenticated NOR integrity bypass
was removed and both original branch instructions were read back before the
restore client exited. Therefore this checkpoint is safe to reuse for
diagnosis, but it must not be renamed as the immutable finished baseline yet.

## NOR diagnosis already closed

`load_nor_data()` reads the exact 5A347 production manifest, sends LLB
separately, and sends an array containing iBoot first followed by the eight
remaining images. The official manifest has ten records total: LLB, iBoot,
DeviceTree, and seven display/recovery images. The non-LLB payload totals
`0x74d30` bytes, below the physical 1 MiB flash capacity.

IDA 9.3 analysis of the exact loaded 5A347 kernelcache identified the called
AppleImage3NORAccess transaction at `0xc0399d94`. Its input validator returns
the same BadArgument result when any of these conditions is true:

- input size is at most `0x400` or at least `0x80000`;
- IMG3 object construction fails;
- the IMG3 type is not the expected type;
- the IMG3 declared full size, rounded to 64 bytes, differs from the external
  method input size rounded to 64 bytes;
- a required DATA/tag boundary or later NOR-layout bound is invalid.

The LLB call passed and the second NOR call failed. Capacity alone is not the
cause. The next experiment should capture `r1`/`r2` and a bounded first header
window at every entry to `0xc0399d94`, then prove whether restored calls the
kernel once per array element or passes a concatenated buffer. Do not bypass
the final NOR program result: first distinguish a Host plist-shape error from
a restored aggregation error and a genuine kernel parser rejection.

The `IOS2NORFlashProbe` in `scripts/ios/restore-iphone3g.py` owns the correct
authenticated entry, integrity path, epilogue, and restore lifecycle. Probe
mode keeps that owner across the first two transactions and records `r1`,
`r2`, a bounded 64-byte header at `r1`, and `lr` at each authenticated entry.
This covers the passing LLB call and the failing call that follows it. Bypass
mode retains the patches across the whole batch but intentionally does not
replace the real NOR program result.

## Rebuild on the cloud Mac

After cloning the pushed commit and placing the external artifacts:

```sh
cd /path/to/qemu-ios-4
scripts/ios/bootstrap-macos.sh
make -C build iphone3g-test
```

The bootstrap selects an available Python 3.11 or newer. Set `PYTHON` only
when a specific interpreter is required on the Host.

The bootstrap creates `.venv`, configures only `arm-softmmu`, enables TCG and
Cocoa, explicitly disables HVF, and builds QEMU. Keep `-accel
tcg,thread=single`; ARM1176 guest code still uses QEMU's TCG JIT translator.
The restore client waits up to 120 seconds for AppleUSBMux by default because
a cold 8C148 boot took about 64 seconds to publish it under single-threaded TCG
on the cloud Mac. Override this only with `--connect-timeout` when diagnosing a
different Host.

## Fresh 8C148 cloud evidence

### 2026-08-28 continuation

The complete 632,025,600-byte ASR transfer again verified its SHA-1, both
Guest `fsck_hfs` runs passed, and the kernelcache was installed.  An
authenticated 8C148 provider-result patch at `0x805a679c` changes only the
post-provider `bne` (`01 d1`) to its success edge (`01 e0`) while retaining
every real provider call.  One live run advanced the complete LLB plus eleven
AP NOR images to 100 percent.  The original branch was authenticated and
restored before the terminal baseband status.

That run ended only after 101 `BBUpdaterExtreme` timeouts with restored status
1014.  A subsequent cold boot proved `_FTLRestore OK`, `FTL_Open [OK]`, a
CRC-valid GUID partition path, and `BSD root: disk0s1`, but the BSD root service
never published.  Offline inspection after the clean QEMU stop found only one
physical version of LPNs 6 through 9; they still held the formatter's
`0x1f`/`0xe0` wipe pattern at the HFS volume-header location.  Thus the valid
filesystem observed by restore remained inside the Guest FTL cache and was not
durably closed before the failed restore reboot.

The exact official restore ramdisk was authenticated and extracted with its
published 8C148 KBAG.  Static ARM analysis closed the baseband decision through
the consumed branch in `restored_external`: function `0xab58..0xada8`, branch
`0xab9c` original `0d00000a`, replacement `0d0000ea`.  This enters restored's
own successful "baseband not requested" path.  The implementation arms it only
after NOR operation 46, restores the NOR patch first, and restores this branch
before accepting terminal status.  Focused Python tests authenticate and
restore both instruction changes byte-for-byte.

A later static and live continuation authenticated both 8C148
AppleImage3NORAccess handlers.  The direct handler is
`0x805a66f8..0x805a67c8`; the catalog handler is
`0x805a6810..0x805a6a70`.  Three narrow provider-result patches retain the real
provider calls and all IMG3 parsing, integrity, and layout checks:

- `0x805a679c`: `01d1` to `01e0`;
- `0x805a6a00`: `1fd1` to `1fe0`;
- `0x805a6a1e`: `01d0` to `c046`.

An early live run appeared to complete LLB plus all eleven NOR images with
those branches, but its tracer had no terminal escape when a later image
failed.  A clean 2026-08-28 replay reset both mutable NAND and NOR from their
authenticated baselines, transferred and SHA-1 verified all 632,025,600
system-image bytes, passed Guest `fsck_hfs` for `Jasper8C148.iPhoneOS` and
`Data`, and installed the kernelcache.  It then returned `0xe00002c2` after
nine printed AP images while the fixed eleven-call tracer waited for entries
that could no longer arrive.  Its process was terminated, all three patched
instruction pairs were read as active, restored byte-for-byte to `01d1`,
`1fd1`, and `01d0`, read back, and detached explicitly from process 1.

`IOS4NORFlashProbe` now arms both authenticated entries between transactions,
selects the matching direct (`0x805a67c4`) or catalog (`0x805a6a64`)
completion, and records the catalog allocation, IMG3, integrity, layout, and
provider-result branches.  A focused fake-GDB test proves mixed-path cleanup,
but a second clean live replay with `--nor-image-limit 10` showed that all ten
bounded objects actually use the direct handler.  Every call returned provider
write `r0=1` and transaction result `r4=0`; the Host restored the NOR patch,
armed and activated the authenticated baseband-success branch, restored that
branch, and received terminal status 1 solely because the required eleventh
image was omitted.  Deriving the probe count from the bounded payload prevents
another fixed-count deadlock.  The next clean restore must trace the complete
eleven-object payload and close the intermittent late direct-handler failure;
do not replace the whole function result or weaken IMG3 validation.

The ten-object terminal run also supplied a new durability witness.  A cold
installed boot reopened FTL successfully, recognized the GUID partition
scheme, and repeatedly resolved `BSD root: disk0s1`, but the BSD partition
service still did not publish.  Therefore a nonzero terminal status after an
intentionally incomplete NOR list is not a reusable installed baseline; retain
the exact erased NAND source and run the next full restore from a fresh clone.

A subsequent full-payload replay closed the remaining nondeterminism before
the AP image list: LLB entered the catalog handler before the probe's original
direct-entry breakpoint, returned `0xe00002c2`, and caused the bounded probe to
time out without any direct observations.  Batch mode now arms both catalog
and direct entries from the start, counts `LlbImageData` as its own transaction
(twelve total for LLB plus eleven AP images), authenticates the complete kernel
windows before applying the same narrow provider-result patches, and retains a
120-second no-entry cleanup bound.  Single-transaction bypass mode still arms
only its requested direct entry.  The complete regression suite after this
change passes 78 qtest subtests and 84 Python tests.  The next live run must
validate this catalog-LLB ownership before the eleven direct images.

The next clean twelve-transaction replay authenticated that ownership and
captured the first deterministic AP-image rejection.  Catalog LLB completed
with provider write `r0=1` and result `r4=0`; direct transactions 2 through 9
also completed with `r0=1`/`r4=0`.  Direct transaction 10 (`0x5174` bytes)
passed construction, IMG3 type, and integrity, but the authenticated layout
helper at `0x805a6264` returned zero.  The consumed `beq` at `0x805a678a`
therefore jumped to the pre-provider `0xe00002c2` result and no later entries
could occur.  The batch timeout printed every observation, restored all
temporary bytes, and detached cleanly.

The runtime owner now authenticates a new exact instruction window around
that decision and changes only `0x805a678a` from `0cd0` to Thumb NOP `c046`.
All IMG3 parsing and integrity checks remain live, and the real provider call
and result remain authoritative.  A new `direct layout result` observation at
`0x805a6788` records the producer value.  Full regression remains 78 qtest
subtests plus 84 Python tests.  The next live restore must prove that this
narrow branch admits transaction 10 and reaches the remaining two AP images.

That replay succeeded completely at the NOR boundary.  Catalog LLB and all
eleven direct AP-image transactions reached their real providers and returned
success; transaction 10 recorded layout helper `r0=0`, continued through the
narrow branch, and its provider returned `r0=1`/transaction `r4=0`.  The final
two images (`0x65b4` and `0x12e74`) also returned `r0=1`/`r4=0`.  The NOR owner
restored every original byte, and the authenticated baseband branch activated
and restored correctly.

Terminal status remained 1 because restored next invoked `update_ir_mcu` and
the real `TiSerialFlasher` reported that this platform has no supported MCU.
Static analysis identified the exact ARM function at `0xeb98..0xed14`: after
the real flasher and error log, instruction `0xecfc` stores the failure result.
`IOS4IRMCUBypass` now authenticates that complete function, changes only
`0xecfc` from `mov r5,#0` (`0050a0e3`) to `mov r5,#1` (`0150a0e3`), waits for
the epilogue, restores the instruction, and detaches before the baseband
handoff.  This represents the explicitly absent external IR MCU rather than
skipping the real call.  Full regression now passes 78 qtest subtests and 85
Python tests.  The next clean full restore must validate IR-MCU completion and
seek terminal status zero.

The first replay with that owner proved an arming race rather than an address
error.  All twelve NOR transactions again succeeded, but restored entered the
TiSerialFlasher retry loop in the short interval after the NOR GDB client
detached and before the IR owner opened a new connection.  UART showed live
`Powercycling` attempts while the Host still waited for the already-passed
`0xeb98` entry.  No IR instruction had been patched.

`probe_direct_batch()` now accepts a successor owner.  At the final NOR
epilogue it restores every NOR patch while the CPU remains stopped, inserts the
IR entry breakpoint on the same GDB connection, transfers ownership, and only
then resumes.  The caller completes and restores the IR owner before processing
later progress messages.  A focused test proves the atomic NOR-to-IR transfer,
all breakpoint cleanup, and byte restoration; full regression is now 78 qtest
subtests and 86 Python tests.  The next live replay must validate this atomic
handoff.

The first atomic-handoff replay was stopped earlier by an intermittent catalog
LLB parser result: construction, IMG3 type, and integrity all returned zero,
but the DATA lookup at `0x805a693c` returned `0x16` and branched to the layout
rejection before any provider call.  This result must not be bypassed because
the helper also owns output pointers that may be invalid on failure.  The
catalog entry observation now captures a bounded 256-byte Guest header/tag
window through `r1`, and the DATA lookup observation captures 32 stack bytes
through `sp`, alongside its return value.  These witnesses will distinguish a
corrupted Host/Guest IMG3 payload from corrupt parser state on the next replay.
The authoritative first-256-byte SHA-256 for the official IPSW LLB is
`58dd65953178c0c181e93c7501e482a49baa861a999d01f273af9ceb05eff9bc`.
Regression remains 78 qtest subtests and 86 Python tests.

The baseband wrapper entry at userspace VA `0xab58` also collided with another
process mapping.  The first live activation stopped at that PC but memory read
returned `E14`; restored then executed the unpatched baseband path and exited
`-1`.  `AuthenticatedRuntimePatch` now treats an entry as owned only when the
complete instruction window matches.  For an unowned fork/exec collision it
removes the breakpoint, single-steps the colliding instruction, reinstalls the
breakpoint, and resumes.  Focused tests cover both this behavior and the
atomic NOR-to-baseband GDB handoff.  A clean run has not yet reached baseband
again because of the catalog failure above.

Three additional full ASR copies transferred and SHA-1 verified all
632,025,600 bytes; both `Jasper8C148.iPhoneOS` and `Data` passed Guest fsck and
the kernelcache installed.  A cold installed boot reaches `FTL_Open [OK]`,
recognizes GPT, and repeatedly resolves `BSD root: disk0s1`, but the BSD service
never registers.  Offline inspection after the clean stop again found exactly
one physical version of LPNs 6 through 9, still containing the formatter's
`0x1f`/`0xe0` pattern.  This confirms that successful terminal restore and FTL
close are required to make the ASR metadata durable; the payload is not yet a
cold-bootable installation.

An official 8C148 restore now completes the full 632,025,600-byte ASR copy,
verifies its SHA-1, passes `fsck_hfs` for both `Jasper8C148.iPhoneOS` and
`Data`, and installs the kernelcache. The first NOR attempt failed on iBoot
because the 8C148 GID oracle covered only DeviceTree, ramdisk, and kernelcache.
The 8C148 manifest now includes the published exact KBAG clear values for
iBoot and every encrypted production NOR asset. Oracle generation validates
each clear record against the wrapped KBAG in the official IPSW.

With the complete oracle, a contaminated development NOR advanced from 8
percent to 91 percent: iBoot, DeviceTree, and the first nine display assets
passed before `batteryfull.s5l8900x.img3` returned `0xe00002c2`. A subsequent
run from the authenticated erased NOR baseline instead rejected the first LLB
transaction (`0x10d74`) with the same result. Treat the 91-percent run as
useful component-key evidence, not a valid clean-flash witness.

Live kernel inspection identified two similar AppleImage3NORAccess handlers.
The first diagnostic incorrectly targeted the helper at `0x805a6810`; the
bounded IMG3 validation helper at `0x805a6574` is also not the write dispatch.
The actual write method begins at `0x805a66f8`, accepts the input pointer and
size in `r1`/`r2`, enforces `0x400 < size < 0x80000`, and returns through
`0x805a67c4`. The current authenticated probe covers its provider lookup, IMG3
parse and integrity checks, provider write result, rejection branches, and
terminal return. Its next live run should classify the clean LLB rejection;
do not weaken the Guest result or revisit already-proven ASR and KBAG paths.
The focused ADM/FMC and AES failure trace remains empty.

8C148 `restored` requested and rewrote `SystemImageData` even when sent false
`CreateFilesystemPartitions` and `SystemImage` options. Do not advertise a
modern image-reuse shortcut without a new live producer/consumer proof.

Set the transferred paths once in every terminal (adjust the IPSW path for the
cloud account):

```sh
export IPHONE3G_FIRMWARE_PROFILE=5A347
export IPHONE3G_IPSW=/path/to/iPhone1,2_2.0_5A347_Restore.ipsw
export IPHONE3G_RUN_DIR=/path/to/qemu-ios-4/.artifacts/runs/5A347-terminal-20260828
export IPHONE3G_DISPLAY=cocoa
```

Resume without repeating the 458 MB ASR transfer. Use four terminals:

```sh
# Terminal 1
make -C build iphone3g-qemu

# Terminal 2, after iBSS exposes recovery USB
make -C build iphone3g-boot

# Terminal 3, after the kernel exposes AppleUSBMux
make -C build iphone3g-usbmuxd

# Terminal 4
make -C build iphone3g-restore-system \
  IPHONE3G_RESTORE_EXTRA_ARGS='--reuse-filesystem-partitions --reuse-system-image'
```

The resume switches are authenticated 5A347 code-path patches. They are valid
here only because this exact checkpoint already passed Guest fsck for both
volumes. A successful retry still requires restored terminal `Status=0`, all
runtime patches restored, and a clean QEMU shutdown.

Then make a writable clone of the terminal NAND and cold-boot it. Require both
root and Data fsck to pass again from the cold boot before freezing the source
NAND read-only as the reusable iOS 2 baseline. Do not validate against a clone
while the source VM is running.

## Return path to 8C148

After the iOS 2 baseline is closed:

1. switch `IPHONE3G_FIRMWARE_PROFILE=8C148` and the official 8C148 IPSW;
2. keep the fixed one-core ARM1176, 128 MiB memory, TCG-only execution, and
   320 by 480 display contract;
3. resume the 8C148 boot chain using the same Make targets and visible Cocoa
   window;
4. validate each service from the live producer/consumer path, not from MMIO
   register tests alone;
5. keep baseband, activation, and retired cloud services explicitly outside
   the locally provable emulator boundary.

The detailed device contracts, known limitations, and debugging commands are
in `docs/system/arm/iphone3g.rst`. `AGENTS.md` contains the accumulated reverse
engineering invariants; preserve them when changing device behavior.

## Hygiene

Do not put IPSWs, decrypted Apple binaries, NAND/NOR images, fuse keys, logs,
or screenshots into Git. Use exact run directories, and remove failed clones,
temporary IDA scripts/logs, sockets, and private temporary files once their
evidence has been summarized. Never delete the checkpoint above until a
replacement has passed the terminal and cold-boot gates.

## 2026-08-28 fresh cloud audit

A fresh audit found that the retained restore-mode UART had progressed beyond
the earlier summary: one boot reached `FTL_Open [OK]`, repeatedly resolved
`BSD root: disk0s1`, and later restore-ramdisk boots executed pid 1 `launchd`
and registered `AppleUSBMux`. The terminal restore still failed during its
in-Guest reboot with the reproducible panic `ADM did not complete command
(POST 0x50)`.

The model published POST and the eight NAND device-ID slots only on a
not-running to running edge. XNU can reinitialize CalmADMFMC without an SoC
reset by rewriting the running control value and then waits for a fresh POST
transaction. `s5l8900_adm_start()` now runs on every control write that sets
`ADM_CONTROL_RUNNING`; the focused qtest clears the prior event and output
buffers, rewrites the same control value, and requires a new POST byte,
device-ID vector, and event interrupt. The test failed against the prior
model and the complete regression now passes 78 qtest subtests plus 86 Python
tests. A clean live restore still must prove terminal status zero and a
subsequent cold installed boot.

The first fresh replay exposed a workflow defect: the default 8C148
`iphone3g-restore-system` target did not pass the already-required authenticated
NOR/IR/baseband owner flags, so it failed on the second direct NOR object.
The 8C148 Make profile now supplies `--probe-ios4-nor-batch`,
`--bypass-ios4-baseband`, and the selected GDB endpoint by default. A following
replay proved catalog LLB plus all eleven AP images through their real provider
calls. Its atomic IR handoff then timed out because the NOR owner's 120-second
socket budget was inherited by the IR owner, while TiSerialFlasher's 100 real
retries take about 225 seconds under single-threaded TCG. The batch now uses
the IR owner's bounded 300-second budget.

Two later full-ASR replays hit the intermittent catalog LLB DATA lookup
failure. In both cases ASR copied and SHA-1 verified all 632,025,600 bytes,
both HFS volumes passed fsck, and the kernelcache installed before errno 22.
The captured kernel buffer begins with the exact official IMG3 bytes, while
static disassembly shows the helper returns 22 only for a malformed tag walk
and leaves its output pointers invalid; this path still must not be bypassed.
The probe now captures the 256-byte buffer again after all IMG3 validation and
captures the 64-byte object plus all four DATA-lookup arguments immediately
before the call. The next failure can therefore distinguish an asynchronous
buffer overwrite from an object-pointer corruption.

The most recent failed restore rebooted without the former `POST 0x50` panic,
restarted pid 1 launchd, and entered FTL recovery. This is live producer proof
for the ADM restart fix. Full regression after these changes passes 78 qtest
subtests plus 86 Python tests; the start-gate unit test now gives the
100-millisecond polling coroutine a one-second outer scheduling budget.

A subsequent clean replay also closed the apparent missing-SecureRoot-record
failure. Static analysis found the six expanded AppleS5L8900XCrypto selectors
`0x835`, `0x899`, `0x836`, `0x837`, `0x838`, and `0x89a`. The live SecureRoot
callback enabled the expanded table, and a stopped-Guest read immediately
before the first NOR crypto request found all six active records, including
`0x836`. The unpatched restore then completed ASR, both fsck passes, and eight
direct NOR images before `batterycharging0.s5l8900x.img3` (`0x5174` bytes)
returned `0xe00002c2`. This proves the earlier missing record was contaminated
run state, not a stable AES-model failure.

The helper at `0x805a6264` is specifically the 128-byte SHSH transformation,
not a generic NOR-capacity check. The AES trace matches the official SHSH
ciphertext word-for-word at the start of each of the first eight direct
images, but the failing ninth request reached the AES DMA input as
`0x000007ff` instead of the IPSW word `0xa1fd6e8b`. The probe now authenticates
that helper and captures its complete 128-byte input immediately before the
crypto call plus its output/result immediately afterward. The next clean
replay must use those witnesses to locate the corruption boundary; the
existing `0x805a678a` branch patch remains diagnostic, not an upstream model
fix.

## 2026-08-30 playable iOS 4 lifecycle

The installed 8C148 image now has a one-command development path:
`make -C build iphone3g-play` prepares the retained authenticated artifacts,
opens a resizable Cocoa window with a visible Host cursor, selects the stable
software MBX backend, and uploads the installed-system boot chain.  It consumes
the persistent `FactoryActivated` record without arming the transient global
`lockdownd` breakpoint; the explicit hacktivated target remains a recovery
fallback.  A live pidfile
gate rejects duplicate launches before they can rotate `boot.log` or occupy the
single-client USB transport.  The original IPSW is no longer needed for an
ordinary boot while all extracted inputs and the KBAG oracle remain present.

The M4 Max live run rendered the complete iOS 4 lock screen at 320 by 480 after
10 minutes 13 seconds of Host time, and a modeled Home press republished it.
LCD format 7, its 1,280-byte stride, the Guest framebuffer, and the software
presentation path remained coherent; an intervening black frame was the
Guest's own disabled panel state, not a Cocoa or MBX failure.

`make -C build iphone3g-shutdown IPHONE3G_SHUTDOWN_TIMEOUT=300` also passed its
live lifecycle gate.  The wrapper omitted absent legacy lockdown plist values,
paired the current connection, started diagnostics relay, and submitted Sleep.
It observed a fresh successful type-`0x43` FTL root after the request before
stopping the authenticated QEMU owner.  The following cold boot reached
`FTL_Open [OK]`, mounted root and data, and started pid 1 launchd without the
former whole-NAND recovery.  The Data volume can still take its bounded safe
`fsck_hfs` pass; the remaining long UI delay is iOS userspace initialization
under translated ARM1176 execution.

Current regression evidence after the display and lifecycle work is the PPP
test, 85 iPhone 3G qtests, and 122 Python tests.  Use
`make -C build iphone3g-wake` for a blanked panel,
`make -C build iphone3g-swipe-unlock` for the real Zephyr2 gesture, and always
use `iphone3g-shutdown` before closing the Cocoa window so the persistent NAND
does not return to dirty recovery.
