Apple iPhone 3G (``iphone3g``)
================================

The ``iphone3g`` machine models the Apple iPhone 3G (``iPhone1,2``,
board ``N82AP``) as a fixed ARM1176JZF-S system.  It is a TCG-only machine;
hardware virtualization is not an execution option for this target.

The implementation is intentionally incremental.  The first machine contract
fixes the single CPU, 128 MiB RAM layout, S5L8900 boot-ROM mapping and VROM
power transition.  It also provides the PL192 and EdgeIC interrupt
controllers, the 12 MHz timer contract, both PL080 DMA controllers, the fixed
LCD scanout, the initial PowerVR MBX wrapper aperture, the initial Synopsys
device-mode USB register, PHY, and endpoint-DMA model, and the
first S5L8900 NAND-controller model.  Initial clock, system-controller, GPIO,
and three-port SPI models are also present, with the application-processor SPI
NOR connected to SPI0.  Five generation-correct S5L8900 UARTs provide the boot
console and a distinct baseband serial endpoint.  The S5L8900 AES DMA path is
implemented for custom, GID, and UID keys, and the SHA-1 accelerator supports
the stock driver's block and continuation-IV contracts.  The ADM register
state machine and 8C148 FMC scatter read, program, and erase actions are
connected to the same NAND backing owner.  Both S5L8900 I2C controllers, the
first PCF50635 PMU contract, and the ISL29003 ambient-light sensor provide the
RTC, battery ADC, persistent GPMEM, and fixed-light sample paths.  The
WM8991-compatible codec control interface is present at its N82AP I2C address,
and both S5L8900 I2S controllers expose their initial register and PL080
request contracts.  Dynamic clock consumers, the remaining board GPIO, I2C,
and SPI endpoints, complete Host audio sample transport, the baseband
processor, and exact NAND ECC remain incomplete.  Nevertheless, the official
8C148 restore and installed-system boot paths have been demonstrated under
single-threaded TCG through an activated, interactive SpringBoard and
AppleMobileDevice publication.  The missing baseband limits cellular service;
it is not represented as working hardware.

Firmware
--------

Apple firmware is not distributed with QEMU.  The supported restore bundle is
``iPhone1,2_4.2.1_8C148_Restore.ipsw``.  Its expected SHA-256 is
``98e5969c3baed660c9a26e94cd7ed4b3cdb7175900f448bcc2223bf885835ce0``.

The initial low-level boot path also requires a legally obtained 64 KiB
S5L8900 boot-ROM image:

.. code-block:: console

   $ qemu-system-arm -M iphone3g -accel tcg,thread=single \
       -bios /path/to/s5l8900-bootrom.bin

The reset vector is an alias of the physical ROM at ``0x50000000``.  Turning
off the VROM power domain removes the reset alias and exposes RAM at address
zero, matching the hardware transition instead of keeping two independent RAM
copies.

The restore bundle's old IMG3 containers distinguish the DATA tag's logical
length from its stored length.  ``scripts/ios/img3_extract.py`` decrypts the
complete AES-CBC blocks, preserves any clear trailing bytes used by early
containers such as the 5A347 DeviceTree, and writes only the logical bytes.
Obtain the IV and key for the selected component from a firmware-key reference
and pass them explicitly:

.. code-block:: console

   $ .venv/bin/python scripts/ios/img3_extract.py encrypted.img3 payload.bin \
       --iv HEX_IV --key HEX_KEY

The extractor validates every root and tag boundary, streams the payload, and
refuses to overwrite an existing destination.  It does not bypass BootROM
signature checks or supply the per-device GID/UID fuse keys used by the AES
accelerator.

Interrupt controllers
---------------------

Two S5L8900-integrated PL192 controllers occupy ``0x38e00000`` and
``0x38e01000``.  VIC1 is daisy-chained into VIC0: the CPU sees only VIC0's
IRQ/FIQ outputs, a ``VICADDRESS`` read at VIC0 acknowledges the selected child
vector, and XNU completes a child interrupt from VIC1 back through VIC0.  The
per-controller daisy priority register is at ``0x28``.  The complete daisy
chain shares one lock so MTTCG producers cannot publish a CPU output computed
from a mixture of old and new controller state.  The stable cold-boot target
still uses single-threaded TCG; ``iphone3g-play-fast`` is an explicit MTTCG
experiment.  A disk-bound ready checkpoint has separately completed its
display and input gates under MTTCG, so quick resume can use that faster mode.

The separate EdgeIC at ``0x38e02000`` latches 64 rising-edge sources in low
and high banks.  Registers ``0x00`` and ``0x04`` enable the two banks;
``0x08`` and ``0x0c`` report pending edges and acknowledge them with
write-one-to-clear.  The bank outputs use the N82AP DeviceTree parent
interrupts 35 and 41, which enter VIC1 inputs 3 and 9.  A source asserted
while masked remains pending and becomes visible when its bank bit is enabled.

PowerVR MBX wrapper
-------------------

N82AP translates the MBX child-bus address ``0x03000000`` to the physical
16 MiB aperture at ``0x3b000000`` and routes its interrupt to VIC0 input 12.
The model provides the wrapper identity, cache-capacity status, command memory,
Guest-programmed GART translation, and the bounded MBX2D and MBX3D command
families observed from iOS 4.2.1.  Successful full-surface writes share one
publication boundary with the LCD double buffers, and render completion is
raised only after the write has become visible.  This live-proven software
path is sufficient for the lock screen, transitions, and SpringBoard; it is
not a general PowerVR implementation.

SPI boot NOR
------------

The three S5L8900 SPI controllers are mapped at ``0x3c300000``,
``0x3ce00000``, and ``0x3d200000`` and route interrupts 9, 10, and 11 to
VIC0.  The application processor uses the physical SST25VF080B on SPI0, with
its active-low chip select driven by GPIO pad 4, pin 0.  This is an 8 Mbit
(1 MiB) serial flash; it is distinct from the 16 MiB NOR-plus-PSRAM package
supporting the baseband processor.

QEMU reuses its upstream ``sst25vf080b`` SSI peripheral.  Attach an exact
1 MiB raw image with:

.. code-block:: console

   $ qemu-system-arm -M iphone3g -accel tcg,thread=single \
       -bios /path/to/s5l8900-bootrom.bin \
       -drive if=pflash,format=raw,file=/path/to/iphone3g-nor.raw

The S5L8900 controller supplies the eight-word transmit and receive FIFOs and
their service interrupts.  Its programmed transfer count covers all bus
words, including words supplied by the transmit FIFO; setup bit 0 separately
enables hardware-generated junk clocks for an RX-only phase.  The stock
driver reads the previous RX batch and refills TX before acknowledging the
old service status, so the model retains newly produced FIFO data and
reasserts both service conditions across that acknowledgement.  The qtest
contract drives the real GPIO chip select, reads JEDEC ID ``bf 25 8e``, and
replays the driver's complete 4,100-byte command-plus-4 KiB read through the
real pflash backing and FIFO refill path.  The stock 8C148
``AppleARMSPIFlashController`` recognizes this exact JEDEC ID; the 512 KiB
``bf 25 8d`` part does not satisfy its probe table and cannot publish the
``IONVRAM`` service required by the PMU backlight driver.

An accepted JEDEC ID is not sufficient by itself.  The last 16 KiB contains
two 8 KiB Apple CHRP NVRAM banks at ``0xfc000`` and ``0xfe000``.  Each bank
has a ``0x5a`` header, a header checksum, an Adler-32 data checksum, and a
nonzero generation.  The bank is a complete CHRP partition stream: the
32-byte Apple ``nvram`` integrity header is followed by the 2 KiB ``common``
variable partition, the 2,064-byte ``APL,OSXPanic`` partition, and the
remaining ``wwwwwwwwwwww`` free partition.  The development Makefile creates
a persistent image with both banks valid and selects the newer second bank
through the stock driver:

.. code-block:: console

   $ make iphone3g-reset-nor

Ordinary ``make iphone3g-prepare`` preserves this image so Guest NVRAM writes
survive a restart.

Multitouch control plane
------------------------

The selected 8C148 DeviceTree and root filesystem identify the N82 touchscreen
as Zephyr2 family ``Z2F52`` on SPI1, not the first-generation Z1 or a later N1
device.  Its active-low chip select is GPIO pad 24 pin 0; power and reset are
pad 7 pin 1 and pad 6 pin 6.  The attention output enters GPIO 155 and its
interrupt group routes to VIC0 input 2.

The initial peripheral model accepts the HBPP constructed-firmware and
calibration data packets, emits the bootloader data, register-read, and
register-write acknowledgements, and transitions to the runtime protocol on
the execute packet.  It reports interface version 1, maximum packet size 660,
family ``0x52``, a 10 by 15 sensor matrix, the region descriptors, and the
4800 by 7200 coordinate domain reported by the generation-matched controller.
Tests exercise these exchanges through the S5L8900 SPI1 FIFOs and separately
prove the attention path through the GPIO interrupt controller.

The 8C148 filesystem's ``Z2F52,1`` entry supplies a 54,156-byte constructed
firmware image with version ``0x0049.bin``.  That Apple payload is not embedded
in QEMU; the Guest driver uploads its own copy.  QEMU absolute pointer events
are converted through the panel's measured ``-75/4656/-75/7275`` surface
bounds.  Left-button press, motion, and release produce the generation-matched
Zephyr2 ``0xcc`` one-contact payload: a 10-byte frame header followed by one
32-byte finger record.  Contact stages 3, 4, and 5 represent begin, move, and
release, and the 8.8 fixed-point coordinates, pressure, and major/minor axes
match the userspace parser.  Each frame raises the real
attention GPIO and is returned through the ``EA``/``EB`` length-then-data
protocol, including both checksums.  The qtest path injects Host events through
QMP and reads all three event phases back through the real SPI1 FIFOs.
It also pulses the active-low reset line after firmware execution and proves
that an application command still completes.  RESET_N clears the current
transaction and queued contacts but does not erase the flashless controller's
powered SRAM; only an LDO power loss returns it to HBPP.
The normal QEMU launch target exposes that same input path on a local QMP
socket.  Once the restored system reaches its lock screen,
``make -C build iphone3g-swipe-unlock`` sends a paced 320 by 480
slide-to-unlock gesture through Zephyr2; it does not alter SpringBoard state or
Guest memory directly.
``make -C build iphone3g-wake`` sends the N82 Home button through QMP when the
Guest has blanked the panel.  ``make -C build iphone3g-qemu-play`` selects that
input path together with a resizable Cocoa window, visible Host cursor,
interpolated scaling, and the live-proven software MBX backend.  On a
Metal-capable macOS Host, ``IPHONE3G_MBX_METAL=1`` explicitly opts into the
narrow experimental Metal offload.

For the usual installed iPhone OS 4.2.1 development loop,
``make -C build iphone3g-play`` combines preparation, the Cocoa QEMU
launch, and the authenticated installed-system upload.  It waits for QEMU's
control plane before opening the single-client USB transport and writes upload
progress to ``.artifacts/runs/iphone3g/boot.log``.  A second invocation refuses
the live pidfile before rotating that log or starting another USB helper.  The
retained extracted firmware is sufficient for later boots; the original IPSW
is needed again only if one of those authenticated inputs is missing.
The ordinary play path consumes the persistent ``FactoryActivated`` data-ark
and hides only the unmodeled baseband nodes; it does not arm the transient
process-wide ``lockdownd`` breakpoint.  Keep
``iphone3g-boot-installed-hacktivated`` as an explicit recovery diagnostic.

After a warm VM has reached a verified interactive screen,
``make -C build iphone3g-checkpoint`` can create an external development
checkpoint.  It freezes the source before outgoing migration so pre-copy
cannot advance from the authenticated visible frame into Auto-Lock, then
copy-on-write clones the exact NAND, NOR, GID, and UID inputs.  It records
their sizes and SHA-256 digests together with the VM state, publishes the
read-only set atomically, and resumes the source VM only if it had been running.
``make -C build iphone3g-resume`` validates that complete manifest, creates a
fresh disposable workspace under ``.artifacts/runs/iphone3g-quick``, and
automatically continues the incoming VM after the frozen stream is loaded.
Never substitute a newer persistent NAND under an older VM state.

Treat a checkpoint as ready only after separate display and input gates pass:
a Cocoa restore must reproduce a complete 320 by 480 screen, the resumed Guest
must consume Zephyr2 records, and its AppleM68Buttons consumer must re-arm the
Power GPIO interrupt after a Host key event.  A later black frame can still be
the Guest's own ``LCD_DISABLE``/Auto-Lock policy.  The
``iphone3g-monitor``, ``iphone3g-screenshot``, ``iphone3g-wake``,
``iphone3g-swipe-unlock``, and ``iphone3g-stop`` targets automatically select
the active quick-resume sockets when the persistent cold-boot VM is absent.
The standard 12-frame swipe provides the observable unlock gate: it must
advance through the status-bar transition and publish the SpringBoard icon grid
and Dock.

ARM1176 checkpoints require both the Secure and Non-secure AArch32 CP15 banks.
QEMU's KVM register decoder intentionally selects the Non-secure bank, whereas
the KVM-shaped migration indexes retain a QEMU-private secure-state bit.  The
VM-state save and load paths preserve that bit separately; otherwise
``DACR_S`` and ``TTBR1_S`` are serialized from the wrong bank and a resumed
kernel recursively faults at the high abort vector.  A stopped-source
round-trip has verified forward SVC/IRQ execution with the corrected
conversion; the interactive-screen gate above remains deliberately separate.

The S5L8900 RTC is not the source of the long cold-boot delay: a live 12 MHz
counter sample advanced by 12,282,204 ticks in one Host second.  The same run
showed roughly 8 ms Guest timer deadlines separated by about 90 ms of Host
translated execution.  A single-thread cold run could therefore spend more
than 40 Host minutes before MBX work, while an MTTCG resume from a mid-boot
checkpoint reached the complete lock screen in about 32 seconds.

Run ``make -C build iphone3g-shutdown`` from another terminal before closing the
window.  It starts a temporary usbmuxd bridge when needed, establishes the
legacy lockdown pairing and authenticated session through pymobiledevice3's
canonical autopair state machine, and submits Diagnostics Sleep.  The helper
enables only the focused ADM/FMC write and failure events, starts from the
current trace offset, and stops the owning QEMU process only after a fresh
type-``0x43`` clean Legacy FTL root is committed.  It stops before iOS enters
the incompletely modeled deep-sleep path.
``make -C build iphone3g-reboot`` performs that storage-clean poweroff and
enters the one-command play path again.  A verified subsequent cold boot loaded
the committed context directly and reached ``FTL_Open [OK]`` without
``_FTLRestore`` or ``Recovering NAND``.  The non-journaled HFS Data volume can
still require its bounded safe ``fsck_hfs`` pass; that is independent of the
much longer whole-NAND FTL recovery avoided by this boundary.  The lower-level
``ios4-shutdown`` client command remains available for diagnostics, but its
``reboot(RB_HALT)`` and USB PHY power-down sequence does not itself commit a
new clean FTL root.

Physical buttons
----------------

The N82AP hold, menu, volume-up, volume-down, and ringer inputs are connected
to GPIO pad 22 pins 5, 0, 1, 2, and 3.  Their independently consumed interrupt
sources are GPIO lines 45, 40, 41, 42, and 43; the model therefore keeps the
physical pin level separate from the interrupt level instead of assuming that
the two DeviceTree indices are interchangeable.  QEMU ``power``, ``home``,
``volumeup``, and ``volumedown`` key events drive the momentary buttons, while
``audiomute`` toggles the ringer switch.  Reset and migration preserve the
idle polarities and switch position, and qtest proves both the data-register
view and level-interrupt view through the board GPIO controller.

Serial ports
------------

The five S5L8900 UARTs start at ``0x3cc00000`` with a ``0x4000`` stride and
route interrupts 24 through 28 to VIC0.  This is a dedicated controller model,
not the later Exynos UART: S5L8900 ``UFSTAT`` has four-bit receive and transmit
counts at bits 0 and 4, with full flags at bits 8 and 9.  The receive path,
16-byte FIFO, reset commands, loopback, CTS status, and polling/IRQ mode used by
early firmware are modeled; transmit completion remains immediate.

UART0 is QEMU serial backend 0 and is the early boot console.  UART4 at
``0x3cc10000`` is backend 1 and is kept separate for the iPhone 3G baseband
link.  For example, capture both without multiplexing their protocols:

.. code-block:: console

   $ qemu-system-arm -M iphone3g -accel tcg,thread=single \
       -bios /path/to/s5l8900-bootrom.bin \
       -serial file:iphone3g-console.log \
       -serial unix:/tmp/iphone3g-baseband.sock,server=on,wait=off

The second backend proves only the application-processor UART endpoint.  A
working cellular stack still requires a separately validated baseband model
and firmware boundary.

AES accelerator
---------------

The S5L8900 AES accelerator at ``0x38c00000`` performs the CBC DMA contract
consumed by early firmware and reports completion on VIC interrupt 39.  The
custom-key path supports AES-128, AES-192, and AES-256.  DMA validation rejects
unusable, oversized, and wrapping transfers.  Register
``0x18`` is the logical transfer length.  Register ``0x28`` is the gather/input
address with its current segment capacity at ``0x2c``; register ``0x20`` is the
scatter/output address with its capacity at ``0x24``.  Input is captured before output
so an in-place request is safe.  The qtest suite verifies AES-128-CBC encrypt
and decrypt against the NIST SP 800-38A vectors through distinct Guest RAM
buffers.  It also verifies noncontiguous input and output segments: status bit
2 requests the next input segment, status bit 1 requests the next output
segment, and CBC chaining state survives both refills until status bit 0 marks
the complete logical transfer.

Completion is delivered through a one-nanosecond, nonzero virtual-time
boundary after the ``GO`` write.  This preserves the 8C148 work-loop ordering:
the request owner is published before VIC interrupt 39 can run its completion
callback.  It also completes at the next virtual-clock opportunity, within the
stock synchronous driver's bounded ``0x2710``-read polling loop.  Because TCG
can execute that loop before returning to its main-loop timer dispatcher, the
first status read also services an already-deferred request.  It never completes
inside the ``GO`` write.  Register ``0x10`` masks status bits 0 through 2 onto
VIC39, and a zero mask must not generate a stale asynchronous callback after a
synchronous request has retired.

The GID group key and per-device UID key are physical fuse material: they are
not contained in an IPSW and QEMU does not provide them.  Supply a legally
obtained 16-byte key through a QEMU secret object rather than a command-line
value.  For example, if ``iphone3g-gid.b64`` contains only the base64-encoded
key:

.. code-block:: console

   $ qemu-system-arm -M iphone3g -accel tcg,thread=single \
       -bios /path/to/s5l8900-bootrom.bin \
       -object secret,id=iphone3g-gid,format=base64,file=iphone3g-gid.b64 \
       -global s5l8900-aes.gid-key-secret=iphone3g-gid

Use ``uid-key-secret`` in the same way for a UID key.  Fuse keys are not
Guest-readable and are deliberately excluded from migration state; a
migration destination must independently provide the same secret.  This
models the accelerator boundary without pretending that firmware files contain
device-unique hardware identity.

The Makefile development flow creates random 16-byte virtual GID and UID keys
once at ``IPHONE3G_RUN_DIR/gid-key.bin`` and
``IPHONE3G_RUN_DIR/uid-key.bin`` and preserves them across boots and NAND
resets.  Keep both files with the VM's persistent NAND and NOR: changing one
changes the virtual hardware identity and invalidates state derived from that
fuse domain.  These are VM-owned identities, not claims about the fuse values
of a physical iPhone or substitutes for Apple's production GID.

Published firmware component keys can close a stock-image boot without
claiming to possess the non-readable GID fuse.  The repository carries
8C148 and 5A347 manifests whose sources are recorded in the files.  Each
combines the selected signed IMG3's encrypted production KBAG with that
component's published clear IV and key.  Exact oracle records take precedence
over the virtual GID, while non-firmware operations such as restore-generated
NOR key derivation remain internally consistent within that VM:

.. code-block:: console

   $ .venv/bin/python scripts/ios/build-kbag-oracle.py \
       scripts/ios/iphone1,2_8C148_kbags.json \
       .artifacts/firmware/8C148/signed \
       .artifacts/firmware/8C148/gid-kbags.bin

For the iPhone OS 2.0 restore profile, use
``scripts/ios/iphone1,2_5A347_kbags.json`` and the matching 5A347 signed
firmware directory instead.  That manifest covers iBoot and every encrypted
display image in the production NOR manifest as well as the restore ramdisk,
DeviceTree, and kernelcache.  ``make iphone3g-firmware`` extracts those exact
signed containers and atomically rebuilds the bundle, so a stale partial
oracle cannot silently fall back to the VM's unrelated virtual GID:

.. code-block:: console

   $ .venv/bin/python scripts/ios/build-kbag-oracle.py \
       scripts/ios/iphone1,2_5A347_kbags.json \
       .artifacts/firmware/5A347/signed \
       .artifacts/firmware/5A347/gid-kbags.bin

Pass the selected generated bundle to the machine:

.. code-block:: console

   $ qemu-system-arm -M iphone3g -accel tcg,thread=single \
       -object secret,id=iphone3g-kbags,format=raw,file=gid-kbags.bin \
       -global s5l8900-aes.gid-kbag-secret=iphone3g-kbags

The oracle answers only an exact configured wrapped/clear KBAG pair; without a
complete GID secret, unmatched GID operations fail.  This keeps public
per-image keys outside the machine model and does not turn them into a
fictitious software GID key.  A complete GID secret, when independently
available, remains the general fallback path.

SHA-1 accelerator
-----------------

The SHA-1 accelerator at ``0x38000000`` follows the register sequence consumed
by the 8C148 ``AppleS5L8900XSHA1`` driver and routes completion interrupt 40 to
VIC1.  The five state words occupy ``0x20`` through ``0x30``; DMA mode, source,
and length are at ``0x80``, ``0x84``, and ``0x8c``.  Reset, interrupt enable,
and interrupt acknowledgement remain independent registers rather than being
folded into the configuration word.

The Guest constructs SHA-1 padding before submitting work, so the model
compresses only complete 64-byte blocks.  Configuration bit 3 selects the five
Guest-written state words as the next block's IV, which is required for the
driver's incremental path.  DMA validation is bounded, checks 32-bit address
wrap, and reads one block at a time.  The qtest contract hashes a 100-byte
message in two submissions, continues from the first hardware result, checks
the standard digest, and acknowledges completion through VIC1.

I2C and power management
------------------------

The two S5L8900 I2C controllers are mapped at ``0x3c600000`` and
``0x3c900000`` and route interrupts 21 and 22 to VIC0.  They use a dedicated
generation-specific model rather than a later Samsung controller.  In
particular, the operation register at offset ``0x20`` reports transfer
completion at bit 8 and start/stop condition changes at bit 13; both are
write-one-to-clear independently of the interrupt-pending bit in ``IICCON``.

The iPhone 3G PCF50635 PMU is attached to I2C0 at seven-bit address ``0x73``.
The implemented subset supports the sequential byte register protocol,
general-purpose memory at ``0x67`` through ``0x76``, the BCD RTC at ``0x59``
through ``0x5f``, and the battery-voltage ADC result consumed by early boot
software.  The default battery input is 4000 mV and can be configured before
realization, for example:

.. code-block:: console

   $ qemu-system-arm -M iphone3g -accel tcg,thread=single \
       -bios /path/to/s5l8900-bootrom.bin \
       -global pcf50635.battery-millivolts=4100

PMU GPMEM and RTC offset survive an application-processor reset, matching a
separate always-on power-management owner rather than ordinary AP MMIO reset
state.  The qtest contract exercises the same repeated-start sequence used by
OpeniBoot and validates address ACK, sequential GPMEM access across AP reset,
the 10-bit 6000 mV ADC encoding, and seven-byte RTC reads.  The PMU's
active-low open-drain output is wired to the DeviceTree GPIO 85.  USB insertion
and removal are latched in INT1 at ``0x02`` when the USB-over-IP host connects
or disconnects.  The five status bytes through ``0x06`` are read-to-clear;
their masks occupy ``0x07`` through ``0x0b`` and suppress the line without
discarding a latched event.  Regulator side effects, other PMU interrupt
events, charger transitions, and shutdown
sequencing remain unimplemented until their exact N82AP contracts are proven.

The ISL29003 ambient-light sensor is attached to I2C0 at its fixed seven-bit
address ``0x44``.  Its eight-byte register file implements command, gain,
threshold, sensor-data, and integration-count access.  A configurable fixed
ambient input defaults to 500 lux; the selected ADC width and range determine
the reported sample.  Its active-low open-drain interrupt is connected to the
DeviceTree GPIO 73 and follows the control-register interrupt flag.  Conversion
latency and changing Host light input are not yet modeled.

The qtest contract also proves that the two open-drain lines are inactive high
after reset, drives an ALS sample outside its programmed threshold, observes
the GPIO 73 to VIC31 route, and clears the same level through the real I2C
control path.

Audio codec control
-------------------

The N82AP codec exposes the WM8991 two-wire control protocol on I2C0.  The
board uses wire address ``0x36``, represented by QEMU as seven-bit address
``0x1b``.  A transaction selects one eight-bit register and transfers 16-bit
big-endian values; multiple complete values auto-increment the register.  The
model provides the documented reset values, returns family ID ``0x8990`` from
register 0, and restores all register defaults when register 0 is written.
Addresses outside the physical ``0x00`` through ``0x3f`` register window read
as zero and do not alias into valid state.

The qtest contract reaches the codec through the S5L8900 I2C controller and
validates address ACK, ID and reset defaults, repeated-start reads, sequential
writes and reads, software reset, and invalid-address isolation.

The two S5L8900 I2S controllers are mapped at ``0x3ca00000`` and
``0x3cd00000``.  Their clock, transmit, receive, command, data, and status
register apertures follow the N82AP contract.  I2S0 is the application audio
path: its transmit and receive requests are PL080 lines 0 and 1 on either DMA
controller.  I2S1 is the baseband audio path and is physically available only
through DMAC1 lines 2 and 3.  Peripheral requests are deferred outside the
register access that asserted them, so PL080 FIFO traffic does not re-enter
the same MMIO transaction.

Tests transfer 16-bit sample words from RAM into each transmit FIFO and read
silence from the I2S1 receive FIFO through the real PL080 request inputs.  They
also prove that I2S1 does not respond through DMAC0.  Transmit data is currently
discarded and receive data is silence: codec serial framing, FIFO occupancy,
codec interrupts, and Host audio input/output remain unimplemented.  Therefore
successful codec discovery and DMA completion are not evidence that iOS audio
services work.

Display
-------

The machine exposes the physical iPhone 3G panel as a fixed 320 by 480 pixel
portrait display.  Its frame clock uses the N82AP timing tuple: a 10.8 MHz
pixel clock, horizontal back/front/sync intervals of 15/15/16 pixels, and
vertical intervals of 4/4/4 lines.  This produces approximately 59.976 Hz;
the Guest cannot resize the panel by programming a different mode.

The S5L8900 LCD controller at ``0x38900000`` supports RGB565, 32-bit RGB888,
and the ARGB8888 format selected by the 8C148 AppleH1CLCD driver.  It scans
windows directly from Guest RAM and raises its frame interrupt through VIC0.
The qtest suite validates the serialized timing registers, interrupt period,
framebuffer stride, color conversion, and the final 320 by 480 screendump.
Panel SPI identification is modeled; backlight power remains bring-up work.

The separate DeviceTree ``tv-out`` provider exposes three ordered 4 KiB
apertures at ``0x39300000``, ``0x39200000``, and ``0x39100000``.  They retain
register writes and reset state so AppleH1DisplayDrivers can map indices 0, 1,
and 2 exactly as serialized.  Encoder, mixer, and TV-out interrupt semantics
are not yet assigned to those banks; they are not aliases of the panel LCD at
``0x38900000``.

DMA and NAND
------------

The machine instantiates the two S5L8900 PL080-compatible DMA controllers at
``0x38200000`` and ``0x39900000``.  Peripheral request inputs are wired into
the upstream QEMU PL080 model and processed across an asynchronous bottom-half
boundary.  NAND uses DMAC0 request 2.  I2S0 uses request lines 0 and 1 on both
controllers; I2S1 uses lines 2 and 3 on DMAC1 only.  Tests cover a real
RAM-to-RAM transfer, NAND FIFO-to-RAM, and both I2S directions, including the
terminal-count interrupt path.

The controller at ``0x38a00000`` models the Toshiba TH58NVG6D1DTG80 fitted to
the 8 GB iPhone 3G configuration.  Its ID bytes are ``98 d5 94 ba`` and its
raw page layout is 4096 data bytes plus 216 spare bytes, with 128 pages per
block, 4096 blocks per bank, and four banks.  Attach a raw data-plus-spare
image of exactly 9,042,919,424 bytes with:

.. code-block:: console

   $ qemu-system-arm -M iphone3g -accel tcg,thread=single \
       -bios /path/to/s5l8900-bootrom.bin \
       -drive if=mtd,format=raw,file=/path/to/iphone3g-nand.raw

A raw sparse-file hole reads as zero and therefore is not an erased NAND
page.  Every unused byte in an image intended to represent erased media must
be initialized to ``0xff``.  The model preserves the page cursor across NAND
status polling so the main and spare areas can be transferred by consecutive
DMA operations, and it implements read, one-way program, and block erase.

Keep an erased image as an immutable baseline and attach only a copy-on-write
clone to QEMU.  On APFS, for example:

.. code-block:: console

   $ .venv/bin/python scripts/ios/reset-iphone3g-nand.py erased.raw
   $ cp -c erased.raw boot-work.raw

The reset helper validates the exact geometry and streams the complete image,
rewriting only modified 4096-plus-216-byte physical pages.  A second run must
report ``repaired_pages=0`` before the baseline is reused.  Restoring only a
visible prefix is insufficient because firmware metadata writes can land in
any bank.

The ECC register and interrupt transport is present, but BCH generation and
correction are not complete: only the all-``0xff`` erased-codeword generation
case is currently accepted.  The development flow has completed an official
restore and writable installed boot against an error-free raw backing image;
it does not claim to inject or correct physical NAND bit errors.

ADM/FMC action protocol
-----------------------

The Apple Data Mover at ``0x38800000`` routes global interrupt 37 to VIC1.
Its control/status register is at ``0x00`` and the shared command/interrupt
status register is at ``0x04``.  The model preserves the 8C148 driver's eight
upload slots, eight event slots, eight upload-action addresses, four
event-action addresses, immediate soft-reset completion, ready/running state,
and the distinct event, command, and upload interrupt acknowledgements.

The selected ``CalmADMFMCFirmware-19`` image uses event action 2 as its main
data section and event action 3 as its bounded pad section.  Instead of
executing the private ADM instruction set, QEMU interprets this generation's
action wire record at the proven firmware boundary.  The command begins at
action 2 plus ``0x1104``; opcode, big-endian page count and pad size, bank
bytes, big-endian page numbers, and big-endian scatter descriptors occupy
offsets ``0x24``, ``0x28``, ``0x2c``, ``0x44``, ``0x244``, and ``0xa44``.
Descriptor processing is streamed, admits all 512 data entries plus their
separate terminator, validates every
32-bit address interval, and requires the producer's zero-length terminator.
At firmware start, action 3 begins with eight little-endian device-ID slots.
The four populated chip-enable slots contain ``0xba94d598`` and the four
absent slots are cleared before the driver consumes them.

Initialization opcode ``0x100`` accepts the firmware producer's zero-page,
zero-pad record and returns success in place.  It has no page-transfer side
effect; subsequent read, program, and erase opcodes own those operations.

Read opcodes ``0x200`` and ``0x300`` fetch each 4096-plus-216-byte page through
the existing NAND device rather than opening or migrating a second backing.
Program opcodes ``0x400`` and ``0x500`` gather the same scatter stream and
bounded action-3 pad prefix, then apply NAND one-way bit transitions through
that backing owner.  Opcode ``0x600`` erases one physical block.  Every
operation updates the in-place result words before raising the event
interrupt.  RUN completion is delivered asynchronously after one microsecond
of virtual time, matching the producer's sleep-and-wakeup contract rather than
re-entering its interrupt handler inside the command-register write.  DMA
publication is direction-sensitive: memory-to-peripheral write DMA completes
one microsecond after ARM_EVENT and is therefore a prerequisite for the
producer's later RUN, while peripheral-to-memory read DMA begins one
microsecond after the RUN action has populated Guest memory.  RUN allows a
one-millisecond window for a late memory-to-peripheral request to appear; if
none does, it publishes the lifecycle event without interpreting the shared
action buffer as a write command.  A memory-to-peripheral terminal count that
precedes RUN proves only DMA readiness: the shared record can be either
incomplete or a syntactically valid stale command, so it never freezes the
transaction.  RUN is the authoritative commit boundary.  It snapshots a
finished pre-RUN DMA immediately, or waits for the final terminal-count
boundary when RUN races an active DMA chain.  The deferred completion is then
bound to that immutable payload, target vector, and command class; its timer
never reparses the shared record after the producer can reuse it.  The
action-boundary implementation does not replay either payload through the
NAND FIFO.  It walks the matching-direction DMAC0 request-2 LLI chain through
the next descriptor that requests terminal count, loads the next active
descriptor, and publishes exactly one terminal-count interrupt.  A later
segment is not consumed until the Guest clears that raw terminal-count state,
so level-triggered completions cannot coalesce.  A qtest fixture replays the
actual 8C148 wire layout against real MTD backing, including both DMA ordering
contracts, linked segments, terminal-count acknowledgement, write, readback,
block erase, and erased-page readback.

Bad-block metadata transformation and full BCH behavior remain unimplemented.
The supported development image therefore assumes error-free media even
though its real 8C148 producer/consumer path is sufficient for restore and
installed-system boot.

SDIO
----

The N82AP SDIO controller is mapped at ``0x38d00000`` and routes interrupt 42
to VIC1.  Its register layout is taken from the selected 8C148
``AppleS5L8900XSDIO`` consumer: command state is at ``0x10``, data status is at
``0x18``, responses start at ``0x20``, and the clock divider, CSR, interrupt,
and DMA-count registers retain their generation-specific gaps through
``0x50``.  In particular, this is not the shorter register interpretation
used by some iPod touch 1G reference emulators.

The iPhone 3G DeviceTree declares no attached SDIO functions.  The model
therefore implements the controller's command-ready, command-start,
command-complete, status-acknowledge, and zero-response path needed for the
stock driver to finish a no-device CMD5 enumeration.  It does not invent an
SDIO Wi-Fi function or claim wireless networking.  The qtest contract replays
the actual driver's polling sequence and verifies that command-ready remains
owned by the controller while command-complete is independently acknowledged.

Clock, power, and GPIO
----------------------

The clock blocks at ``0x38100000`` and ``0x3c500000`` expose the reset PLL and
divider tuple consumed by early S5L8900 software.  Their writable register
state is migratable, but those values do not yet retime every downstream
device; the fixed timer and panel contracts remain their independently proven
rates until a live BootROM producer trace closes the dynamic relationship.

The system-controller aperture at ``0x39a00000`` owns both the VROM power
transition and seven GPIO interrupt groups.  The main GPIO block at
``0x3e400000`` exposes 25 pads of eight pins, including function-select, data,
pull, and sleep registers.  Edge history and live level status are separate:
writing one to interrupt status clears an edge, while an active level remains
asserted.  The seven group outputs route to the N82AP VIC inputs
``33, 32, 31, 3, 2, 1, 0``.  Tests exercise both interrupt modes and the
pad/pin function-select output path.  Physical buttons, panel control, and
other board endpoints still need to be attached to their proven pins.

The two-register watchdog at ``0x3e300000`` uses the reset peripheral clock
derived from the same N82 clock tuple.  It implements the 11-bit counter,
prescaler, clock selector, clear and disable keys, interrupt-enable path to
VIC1 input 19 (global interrupt 51), and QEMU watchdog action on a reset-mode
expiry.  The stock 8C148 platform driver writes ``0x001f4a00`` for its normal
reload and ``0x00100000`` for the fast reboot path.  Tests advance QEMU's
virtual clock to the exact normal expiry boundary, acknowledge it by reloading,
and prove that the disable key prevents a later interrupt.

USB-over-IP development transport
---------------------------------

For the repository's current development boot sequence, use three terminals:

.. code-block:: console

   $ make -C build iphone3g-qemu
   $ make -C build iphone3g-boot
   $ make -C build iphone3g-log

``iphone3g-qemu`` creates a persistent copy-on-write NAND image under the
repository's ignored ``.artifacts/runs/iphone3g`` directory when needed and
stays in the foreground.
Use ``make -C build iphone3g-reset-nand`` only when a fully erased NAND is
intended.  Run ``iphone3g-boot`` only after iBSS reaches its recovery prompt. Use
``make -C build iphone3g-help`` for path, port, display, and boot-argument
overrides.  ``make -C build iphone3g-monitor`` opens the QEMU monitor, and
``make -C build iphone3g-screenshot`` captures its 320 by 480 scanout.
Start ``iphone3g-usbmuxd`` in a fourth terminal and run
``iphone3g-restore-system`` for a fresh official restore.  After a terminal
restore status of zero, stop QEMU cleanly and preserve the NAND, NOR, GID, and
UID files as one virtual-device identity.  On later boots, replace
``iphone3g-boot`` with ``iphone3g-boot-installed`` to select ``disk0s1``.
An opt-in development boot can apply the Legacy iOS Kit 8C148 hacktivation
branch after Apple's signed ``lockdownd`` has been accepted and mapped:

.. code-block:: console

   $ make -C build iphone3g-boot-installed-hacktivated

The helper arms raw virtual address ``0x100a0`` before ``bootx``, waits for
the first execution in ``lockdownd``, authenticates the adjacent 8C148
instructions, and replaces ``bne 0x100bc`` with ``b 0x10800`` in Guest
memory.  It then detaches explicitly from process 1 so the VM resumes.  The
signed file and persistent NAND are not modified.  This recipe was
independently reduced from ``Legacy-iOS-Kit`` commit
``03e0fdfb18f07cd3c0867a38ea05149208cfc551``: its stock input SHA-256 is
``c6af75ce49bd55f15b048f5b3559685ab4581cc222a647bee88ae4c49d67cff1``
and its patched output SHA-256 is
``79e2a3356fa922471c4dfec1e9d4776dbaea27493cfd25f755f466f507b76913``.
The external BSDIFF payload is GPLv3 and is not copied into QEMU.  This is a
development compatibility workaround, not a cellular/baseband implementation
or an Apple activation service.

To persist the smaller Legacy iOS Kit factory-activation record instead of
patching ``lockdownd`` on every boot, start QEMU against an already restored
development NAND and run:

.. code-block:: console

   $ make -C build iphone3g-install-activation

This boots Apple's signed 8C148 restore ramdisk, stops at an authenticated
``restored_external`` entry, and calls only that image's own ``fsck_hfs``,
``mount_hfs``, stdio, ownership, and unmount paths.  It writes and byte-verifies
the 286-byte ``/mnt2/root/Library/Lockdown/data_ark.plist`` containing
``-ActivationState=FactoryActivated`` and ``-BrickState=false``.  The bounded
scratch area, all registers, and both raw-address breakpoints are restored
before the process-aware GDB detach.  The target never replaces a signed Guest
executable and ordinary ``iphone3g-prepare`` or boot targets preserve the
result.  Full NAND erasure remains the separate ``iphone3g-reset-nand`` action.

Rootless development network
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The stock 8C148 image publishes an Apple USB Ethernet function, but publishing
its descriptors is not proof of a working link.  The optional diagnostic
``IPHONE3G_USBMUXD_ETHERNET_PROBE=1`` selects configuration 4 and interface 2
alternate setting 1.  On the validated image, the Guest reports carrier state
``3`` rather than the active value ``4`` and never arms bulk endpoints
``0x05/0x86``.  The default usbmux path therefore keeps its original
configuration and does not repeatedly poll that inactive interface.

For a network path compatible with the shipped XNU and drivers, the repository
can run the Guest's own signed ``/usr/sbin/pppd`` over N82 UART4.  Stop QEMU
before enabling or disabling the job:

.. code-block:: console

   $ brew install libslirp
   $ make -C build iphone3g-enable-ppp
   $ make -C build iphone3g-qemu-ppp

In a second terminal, start the unprivileged Host peer and NAT, then use the
normal installed-system boot target in a third:

.. code-block:: console

   $ make -C build iphone3g-ppp-nat
   $ make -C build iphone3g-boot-installed-hacktivated

The provisioner authenticates the exact inert
``com.apple.chud.pilotfish.plist`` record, requires a unique current FTL
logical page, and replaces exactly 530 bytes with an equal-sized ``pppd`` job.
It does not change the HFS catalog, the remainder of the 4096-byte data page,
or any NAND spare byte.  ``make -C build iphone3g-disable-ppp`` restores the
exact stock bytes, and both operations are idempotent.  The peer implements
async HDLC, LCP, IPCP address and DNS negotiation, assigns ``10.0.2.15`` with
peer ``10.0.2.2`` and DNS ``10.0.2.3``, and translates complete IPv4 datagrams
through libslirp without a TUN device or root privileges.  Set
``IPHONE3G_PPP_WITNESS_PACKETS=1`` to make the peer exit successfully only
after it observes at least one Guest IPv4 request and one NAT response.

The PPP framing and negotiation core is retained from the MIT-licensed S5LBox
implementation, including its RFC and live-packet provenance.  The NAND
provisioner, UART socket integration, libslirp boundary, packet counters, and
failure gates are local to this tree.

Slow single-threaded hosts can give every framed USB request an explicit
bound, for example
``IPHONE3G_BOOT_EXTRA_ARGS=--usb-request-timeout\ 120000``.  A request timeout
closes that framed connection before rediscovery so a late response cannot be
mistaken for the next request.
The 5A347 Make profile transiently replaces restored's security-wipe call with
a zero result.  It still creates the partition map, waits for the IOMedia
remount, and runs ``newfs_hfs`` for both filesystems.  The helper authenticates
the function entry, wipe call, and epilogue before the change, restores the
original call at the epilogue, and formally detaches the debugger before ASR.
If a forked tool maps its own instruction at the same virtual epilogue address,
the helper rejects that mapping, temporarily removes the breakpoint, executes
the colliding instruction with one GDB single-step, and reinstalls the
breakpoint for restored's authenticated mapping.
This avoids a 500 MiB all-``0xff`` pass that is redundant for a dedicated,
freshly reset emulator NAND while preserving the structures ASR consumes.
The same profile authenticates the 5A347 kernel's complete
``AppleImage3NORAccess`` transaction and transiently disables its two IMG3
integrity-failure branches.  It does not replace the NOR program result: the
real pflash-backed writer must still succeed for every image in the NORData
batch.  The debugger retains restore ownership across those repeated kernel
calls; when restored publishes its terminal status, it stops the Guest,
authenticates both patched instructions again, restores and reads back the
original branches, and only then detaches.  The signed input on disk is never
changed.
After a prior restore has successfully formatted both filesystem partitions,
resume it with
``IPHONE3G_RESTORE_EXTRA_ARGS=--reuse-filesystem-partitions`` to avoid
repeating restored's full partition wipe.  A live protocol 11 run recreated
the partitions even when the Host sent ``CreateFilesystemPartitions=false``,
so the 5A347 profile authenticates the consumer's decision and return windows,
briefly selects its zero-result path, and restores the original instructions
before restore continues.  Do not use the option on a blank or partially
partitioned NAND image.  This explicit override replaces the 5A347 profile's
first-restore default ``--skip-filesystem-wipe``; the two authenticated modes
are deliberately never submitted together.
If the preserved 5A347 NAND has already passed ``fsck_hfs`` for both volumes,
append ``--reuse-system-image`` to the same override.  The 5A347 restored
consumer returns directly from ``restore_images`` when its local
``SystemImage`` option is false, but protocol 11 does not persist the Host's
nested false value into the local options plist.  The 5A347 profile therefore
authenticates both the partition and system-image decision/return windows,
briefly selects their existing zero-result paths, and restores every changed
instruction before the normal mount, ``fixup_var``, firmware, and NVRAM
stages.  The helper accepts this resume mode only together with
``--reuse-filesystem-partitions``; it must not be used to turn an unverified
or incomplete filesystem into a nominally successful restore.
The boot helper uses QEMU's local GDB endpoint to authenticate iBEC's original
40-byte boot-argument buffer, writes the requested value, verifies the exact
readback, and accepts the handoff only after new kernel UART output appears.
For a deterministic early-kernel debugging boundary, run the boot target with
``IPHONE3G_BOOT_EXTRA_ARGS=--wait-before-bootx``.  After it reports that the
kernel is loaded, attach LLDB to the configured GDB port, set raw-address
breakpoints, resume the CPU, and then press Enter in the boot terminal.  The
Cocoa display remains visible throughout this sequence.

After ASR has written a system image, compare every recoverable FTL user page
with the extracted raw HFS partition.  For a transfer that ends inside its
last page, pass the exact byte count printed by ASR so the untransferred tail
is not reported as NAND loss:

.. code-block:: console

   $ make -C build iphone3g-analyze-restore \
       IPHONE3G_RESTORE_SOURCE=/path/to/root-filesystem.hfs \
       IPHONE3G_RESTORE_SOURCE_OFFSET=36864 \
       IPHONE3G_RESTORE_SOURCE_BYTES=458469888

The analyzer scans the complete data-plus-spare NAND geometry, accepts user
page types ``0x40`` and ``0x41``, and maps source page zero to FTL logical page
63.  ``IPHONE3G_RESTORE_SOURCE_OFFSET`` selects the HFS partition when the
source is a whole-disk image; the 5A347 UDRW image places HFSX at sector 72,
or byte 36864.  It reports missing logical ranges separately from ranges for
which only nonmatching physical versions survive.  A restore is complete only
when every complete transferred source page has at least one byte-exact NAND
version.
To capture the corresponding ADM transaction evidence without tracing routine
boot traffic, start the restore client with ``--start-restore-gate``, enable
``iphone3g-trace-restore-on`` after ``FTL_Open``, and then open the gate.  The
target records one line per committed NAND page plus failed or skipped write
boundaries in ``IPHONE3G_TRACE``.  Run ``iphone3g-trace-restore-off`` before an
unrelated long-lived workload if the VM remains active.

Validated iOS 4.2.1 boundary
----------------------------

The reproducible 8C148 run completed all 2,303 ASR chunks and wrote exactly
632,025,600 system-image bytes.  The Guest verified the transfer, passed both
root and data ``fsck_hfs`` checks, installed the kernelcache, and completed the
catalog LLB plus eleven application-processor NOR transactions through their
real providers.  Every authenticated temporary restore patch was restored and
read back before the Host accepted restored status zero.  The focused ADM/FMC
and AES failure trace was empty.

A subsequent cold invocation used the persistent Legacy data-ark record with
the normal watchdog and the same single ARM1176 TCG CPU.  It did not apply the
runtime ``lockdownd`` hacktivation branch.  Older dirty runs can still perform
the bounded ``_FTLRestore`` path, but a fresh Diagnostics Sleep commit wrote
the complete child-page chain and final type-``0x43`` root.  The next cold boot
loaded that context, omitted both ``_FTLRestore`` and ``Recovering NAND``, then
reported ``FTL_Open [OK]`` and ``BSD root: disk0s1``.  Launchd starts, the USB
controller publishes
``PTP + Apple Mobile Device + Apple USB Ethernet``, and usbmux identifies
build ``8C148`` and product version ``4.2.1``.  ``lockdownd`` reports
``ActivationState=FactoryActivated`` and accepts a trusted lockdown session.
The 320 by 480 LCD scanout displays the real iOS 4 lock screen.  A paced Host
swipe is consumed as 12 Zephyr2 samples and visibly drives the unlock
transition.  The driver's generation-matched ``ResetWhenExitingUILock`` policy
then pulses RESET_N; the device model preserves the already downloaded
application image across that reset, as the real powered controller does.
The same persistent VM subsequently published a stable SpringBoard home screen
with status bar, icons, and Dock.  Direct saved-context loading is now the
verified ordinary restart path; a ready-state checkpoint remains the faster
inner development loop when a full hardware boot is unnecessary.

The bounded software MBX3D path now decodes the measured tiled, status, solid,
and textured-sprite object-list families through the Guest-programmed GART.
It accepts the observed 1024 by 1024 allocation header with an independently
bounded 320 by 480 UV footprint and row pitch, and publishes render completion
only after a nonzero virtual-clock boundary.  A live 8C148 run rendered the
status bar, lock screen, and power-off overlay from Guest data and accepted 28
full-height unlock-transition submissions without a decoder rejection.  A
later cold run completed 12 MBX2D batches and 16 MBX3D sprite/status jobs with
no decoder rejection before the Guest explicitly cleared LCD ``VIDCON0`` bit
0.  A further persistent-activation cold run traced each GART render target to
the physical LCD double buffers at ``0x0f496000`` and ``0x0f52c000``.  Its
fixed-size screendump captured the complete iOS 4 lock screen while the panel
was enabled; a later black screendump followed an explicit Guest
``LCD_DISABLE`` write and is therefore a display-policy state, not an MBX
rendering failure.  The software PowerVR command path is now live-Guest proven;
the optional Metal offload remains experimental and separately gated below.
The paced Zephyr2 gesture and the stable post-unlock home-screen presentation
have both been observed through this path.
Apple USB Ethernet is published but does not yet constitute an end-to-end IP
witness.  The rootless PPP/libslirp path is implemented and locally unit-tested,
but its bidirectional live-Guest packet witness is still required before
network connectivity is claimed.  Cellular service, Wi-Fi, and end-to-end
audio remain outside the demonstrated boundary.

An opt-in macOS backend, selected with ``IPHONE3G_MBX_METAL=1``, submits the
already decoded, bounds-checked status and solid source-over pixel operation to
an integer Metal compute kernel.  The kernel compiles at device realization,
runs a four-pixel bit-exact self-test, and records each successful Guest-backed
submission as ``s5l8900_mbx_metal_complete``.  Command parsing, texture
sampling, GART transactions, interrupt ordering, and unsupported command
rejection remain in the device model; this is a narrow experimental offload,
not a claim that PowerVR MBX is generally translated to Metal.  Runtime command
failure is traced and falls back to the bit-identical software equation.  A
Host that has the Metal framework but no Metal device rejects the opt-in at
startup.  A later Apple M4 Max run completed the realization self-test and
eight live Guest-backed submissions without a Metal failure or MBX decoder
rejection.  Its first complete lock screen appeared after 269 Host seconds;
the following black frame was paired with the Guest removing the active LCD
window, writing ``LCD_DISABLE=1``, and clearing ``VIDCON0`` bit 0.  A modeled
Home press republished the lock screen.  These observations prove the narrow
Metal path and distinguish normal Guest display policy from a renderer stall;
they do not widen the supported PowerVR command families.

The optional ``IOSU`` transport presents QEMU's emulated device to the pinned
``pymobiledevice3`` in the repository's ``.venv``.  Start a TCP server chardev
and bind it to the board's internal USB controller:

.. code-block:: console

   $ build/qemu-system-arm -M iphone3g -accel tcg,thread=single \
       -bios /path/to/s5l8900-bootrom.bin \
       -chardev socket,id=usboip,host=127.0.0.1,port=1337,server=on,wait=off \
       -global s5l8900-usb.chardev=usboip

In a second terminal, run the wrapper with the same repository environment:

.. code-block:: console

   $ .venv/bin/python scripts/ios/pymobiledevice3-usboip.py \
       --usboip 127.0.0.1:1337 restore shell

The QEMU transport closes real ``IRecv`` discovery and configuration against
the iPhone1,2 DFU identity.  Control and bulk requests traverse the
Guest-programmed endpoint DMA buffers and complete through endpoint interrupt
state; ``scripts/ios/tests/test_qemu_usboip.py`` validates this with one live
QEMU/``IRecv`` session.  Enumeration retains a short socket timeout so a stale
first connection after Guest re-enumeration can close and enter ``IRecv``'s
retry loop; the successfully enumerated socket then switches to blocking mode
for long control and bulk transfers.  The adapter also weakly tracks the
current PyUSB-shaped device and closes it before rediscovery.  This recovers
when ``IRecv`` construction fails after discovery but before its normal
resource-disposal path, without retaining a successful context's socket.
This proves the transport mechanism, but it is not by itself evidence that an
unmodified BootROM or restore image has executed it.

The PMU property is explicit because MBCS1 bit 0 is the read-only USB-power
presence input consumed by the recovery idle-off task; omit it to model an
unplugged phone.
