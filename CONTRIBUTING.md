# Contributing to qemu-ios-4

This repository is an experimental QEMU fork. Pull requests for the `iphone3g`
machine, its S5L8900 devices, tests, documentation, and development tools are
welcome here. Changes unrelated to the iPhone 3G should normally be sent to
the upstream QEMU project.

## Before opening a pull request

- Keep Apple firmware, boot ROMs, NAND images, pairing records, and fuse keys
  out of commits, issues, and CI artifacts.
- Preserve the fixed `iPhone1,2` hardware contract. Add another machine type
  rather than silently widening this board to a different generation.
- Include a qtest or focused tool test for observable behavior changes.
- Run `make -C build iphone3g-test` and `git diff --check`.
- Update `docs/system/arm/iphone3g.rst` when a supported workflow or hardware
  boundary changes.
- Follow QEMU's coding style and add a `Signed-off-by` line as described by
  QEMU's Developer Certificate of Origin process (`git commit -s`).

Bug fixes should explain the producer/consumer evidence behind a hardware
contract. Diagnostic patches to Guest memory must authenticate the original
bytes, remain bounded to a supported firmware build, and restore the original
bytes before teardown.

## Reporting problems

Use the repository's iPhone 3G issue form. Include the Host OS and architecture,
QEMU commit, firmware build identifier, exact command, and the smallest useful
tail of the UART or focused trace log. Redact serial numbers, UDIDs, Host paths,
pairing material, and all secret values. Do not upload vendor firmware or a
Guest storage image.
