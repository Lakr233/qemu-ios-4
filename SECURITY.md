# Security policy

## Reporting a vulnerability

Do not open a public issue for a vulnerability. Use GitHub's private
vulnerability reporting for this repository, or contact the maintainer listed
for the iPhone 3G machine in `MAINTAINERS`.

Include the affected commit, Host platform, impact, and a minimal reproducer.
Never send Apple firmware, NAND images, pairing records, device identifiers,
GID/UID keys, or other device-derived secrets. A reproducer should use synthetic
data whenever possible.

## Supported code

Security fixes are applied to the current `master` branch. The iPhone 3G model
processes untrusted Guest-controlled DMA descriptors, MMIO values, USB frames,
NAND metadata, and firmware containers; reports involving bounds violations,
Host memory safety, unsafe path handling, or secret disclosure are in scope.

The project is experimental and provides no sandbox boundary beyond the one
offered by QEMU itself. Run untrusted images with the same isolation you would
use for any other emulator workload.
