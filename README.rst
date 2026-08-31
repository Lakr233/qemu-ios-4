=====================
qemu-ios-4
=====================

``qemu-ios-4`` is an experimental QEMU fork that models the Apple iPhone 3G
(``iPhone1,2`` / ``N82AP``) well enough to restore and run iPhone OS 2.0 and
iOS 4.2.1.  The verified iOS 4 path reaches an interactive SpringBoard with a
resizable Cocoa display, mouse-driven multitouch, Home and Power buttons,
persistent NAND/NVRAM, USB device transport, and an optional rootless PPP
network path.

This is a research and preservation project, not a complete iPhone emulator.
The baseband, telephony, camera, host audio output, and exact NAND ECC are not
implemented.  The normal display path uses the software MBX renderer; the
Metal backend is an explicit experiment.  Full cold boots use ARM TCG and can
be slow, while a verified ready-state checkpoint provides the practical inner
development loop.

Getting started
===============

The complete machine contract, supported firmware hashes, first restore,
installed boot, checkpoint, clean shutdown, and troubleshooting procedures are
in the `iPhone 3G machine documentation
<docs/system/arm/iphone3g.rst>`_.  The end-to-end graphical workflow is
verified on macOS.  The device model and qtests also build on Linux.

On macOS, install the build and optional bridge dependencies, then run:

.. code-block:: shell

   brew install autoconf automake glib libimobiledevice libimobiledevice-glue \
       libplist libslirp libtool libusb ninja pixman pkg-config python@3.13

Python 3.11 or newer is supported.  Build and verify the project with:

.. code-block:: shell

   ./scripts/ios/bootstrap-macos.sh
   ./scripts/ios/fetch-firmware.sh
   make -C build iphone3g-help
   make -C build iphone3g-test

The initial system restore is a stateful, multi-terminal procedure; follow the
machine documentation rather than interrupting it mid-transfer.  Once an
installed virtual-device identity exists, ``make -C build iphone3g-play``
starts the normal resizable Cocoa session.  ``iphone3g-checkpoint`` and
``iphone3g-resume`` provide the faster repeated-development path.

The firmware script downloads the unmodified supported IPSW directly from
Apple over HTTPS and verifies its exact size and SHA-256.  No Apple firmware,
boot ROM, NAND image, fuse key, or ready-state checkpoint is stored in this
repository.  Generated material remains under the ignored ``.artifacts/``
directory.

Project boundaries
==================

Apple, iPhone, and iOS are trademarks of Apple Inc.  This project is not
affiliated with or endorsed by Apple.  Users are responsible for obtaining and
using firmware and device-derived material lawfully.  Do not attach firmware,
NAND images, pairing records, or fuse keys to public issues.

Fork-specific bug reports and pull requests are welcome on this repository.
Read `CONTRIBUTING.md <CONTRIBUTING.md>`_ before submitting changes and use
the process in `SECURITY.md <SECURITY.md>`_ for vulnerabilities.  General QEMU
issues that reproduce without the ``iphone3g`` machine still belong upstream.

Third-party code
================

The UART PPP protocol core and bounded MBX decoders contain code derived from
`S5LBox <https://github.com/j0shua-SYSON/S5LBox>`_ commit
``6f203ba550b49afadee008c7eb55373a838eed33`` under the MIT License.  The
original copyright notices remain with those sources, and the permission text
is included in ``LICENSES/MIT.txt``.  Other device contracts
independently corroborated against S5LBox identify that provenance in their
file headers.  The rest of the emulator follows the per-file licensing
described by QEMU's root ``LICENSE`` file.

===========
QEMU README
===========

QEMU is a generic and open source machine & userspace emulator and
virtualizer.

QEMU is capable of emulating a complete machine in software without any
need for hardware virtualization support. By using dynamic translation,
it achieves very good performance. QEMU can also integrate with the Xen
and KVM hypervisors to provide emulated hardware while allowing the
hypervisor to manage the CPU. With hypervisor support, QEMU can achieve
near native performance for CPUs. When QEMU emulates CPUs directly it is
capable of running operating systems made for one machine (e.g. an ARMv7
board) on a different machine (e.g. an x86_64 PC board).

QEMU is also capable of providing userspace API virtualization for Linux
and BSD kernel interfaces. This allows binaries compiled against one
architecture ABI (e.g. the Linux PPC64 ABI) to be run on a host using a
different architecture ABI (e.g. the Linux x86_64 ABI). This does not
involve any hardware emulation, simply CPU and syscall emulation.

QEMU aims to fit into a variety of use cases. It can be invoked directly
by users wishing to have full control over its behaviour and settings.
It also aims to facilitate integration into higher level management
layers, by providing a stable command line interface and monitor API.
It is commonly invoked indirectly via the libvirt library when using
open source applications such as oVirt, OpenStack and virt-manager.

QEMU as a whole is released under the GNU General Public License,
version 2. For full licensing details, consult the LICENSE file.


Documentation
=============

Documentation can be found hosted online at
`<https://www.qemu.org/documentation/>`_. The documentation for the
current development version that is available at
`<https://www.qemu.org/docs/master/>`_ is generated from the ``docs/``
folder in the source tree, and is built by `Sphinx
<https://www.sphinx-doc.org/en/master/>`_.


Building
========

QEMU is multi-platform software intended to be buildable on all modern
Linux platforms, OS-X, Win32 (via the Mingw64 toolchain) and a variety
of other UNIX targets. The simple steps to build QEMU are:


.. code-block:: shell

  mkdir build
  cd build
  ../configure
  make

Additional information can also be found online via the QEMU website:

* `<https://wiki.qemu.org/Hosts/Linux>`_
* `<https://wiki.qemu.org/Hosts/Mac>`_
* `<https://wiki.qemu.org/Hosts/W32>`_


Submitting patches
==================

The QEMU source code is maintained under the GIT version control system.

.. code-block:: shell

   git clone https://gitlab.com/qemu-project/qemu.git

When submitting patches, one common approach is to use 'git
format-patch' and/or 'git send-email' to format & send the mail to the
qemu-devel@nongnu.org mailing list. All patches submitted must contain
a 'Signed-off-by' line from the author. Patches should follow the
guidelines set out in the `style section
<https://www.qemu.org/docs/master/devel/style.html>`_ of
the Developers Guide.

Additional information on submitting patches can be found online via
the QEMU website:

* `<https://wiki.qemu.org/Contribute/SubmitAPatch>`_
* `<https://wiki.qemu.org/Contribute/TrivialPatches>`_

The QEMU website is also maintained under source control.

.. code-block:: shell

  git clone https://gitlab.com/qemu-project/qemu-web.git

* `<https://www.qemu.org/2017/02/04/the-new-qemu-website-is-up/>`_

A 'git-publish' utility was created to make above process less
cumbersome, and is highly recommended for making regular contributions,
or even just for sending consecutive patch series revisions. It also
requires a working 'git send-email' setup, and by default doesn't
automate everything, so you may want to go through the above steps
manually for once.

For installation instructions, please go to:

*  `<https://github.com/stefanha/git-publish>`_

The workflow with 'git-publish' is:

.. code-block:: shell

  $ git checkout master -b my-feature
  $ # work on new commits, add your 'Signed-off-by' lines to each
  $ git publish

Your patch series will be sent and tagged as my-feature-v1 if you need to refer
back to it in the future.

Sending v2:

.. code-block:: shell

  $ git checkout my-feature # same topic branch
  $ # making changes to the commits (using 'git rebase', for example)
  $ git publish

Your patch series will be sent with 'v2' tag in the subject and the git tip
will be tagged as my-feature-v2.

Bug reporting
=============

The QEMU project uses GitLab issues to track bugs. Bugs
found when running code built from QEMU git or upstream released sources
should be reported via:

* `<https://gitlab.com/qemu-project/qemu/-/issues>`_

If using QEMU via an operating system vendor pre-built binary package, it
is preferable to report bugs to the vendor's own bug tracker first. If
the bug is also known to affect latest upstream code, it can also be
reported via GitLab.

For additional information on bug reporting consult:

* `<https://wiki.qemu.org/Contribute/ReportABug>`_


ChangeLog
=========

For version history and release notes, please visit
`<https://wiki.qemu.org/ChangeLog/>`_ or look at the git history for
more detailed information.


Contact
=======

The QEMU community can be contacted in a number of ways, with the two
main methods being email and IRC:

* `<mailto:qemu-devel@nongnu.org>`_
* `<https://lists.nongnu.org/mailman/listinfo/qemu-devel>`_
* #qemu on irc.oftc.net

Information on additional methods of contacting the community can be
found online via the QEMU website:

* `<https://wiki.qemu.org/Contribute/StartHere>`_
