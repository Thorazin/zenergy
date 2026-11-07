# Zenergy kernel driver

Based on AMD_ENERGY driver, but with some jiffies added so non-root users can read it safely.

Kernel driver zenergy
==========================

  Prefix: 'zenergy'

  Addresses used:  RAPL MSRs

  Datasheets:

  - Processor Programming Reference (PPR) for AMD Family 17h Model 01h, Revision B1 Processors

	https://developer.amd.com/wp-content/resources/55570-B1_PUB.zip

  - Preliminary Processor Programming Reference (PPR) for AMD Family 17h Model 31h, Revision B0 Processors

	https://developer.amd.com/wp-content/resources/56176_ppr_Family_17h_Model_71h_B0_pub_Rev_3.06.zip

  - Preliminary Processor Programming Reference (PPR) for AMD Family 19h Model 1h, Revision B1 Processors

	https://www.amd.com/system/files/TechDocs/55898_pub.zip

Author: Naveen Krishna Chatradhi <nchatrad@amd.com>

Description
-----------

The Energy driver exposes the energy counters that are
reported via the Running Average Power Limit (RAPL)
Model-specific Registers (MSRs) via the hardware monitor
(HWMON) sysfs interface.

1. Power, Energy and Time Units
   MSR_RAPL_POWER_UNIT/ C001_0299:
   shared with all cores in the socket

2. Energy consumed by each Core
   MSR_CORE_ENERGY_STATUS/ C001_029A:
  Read-only cumulative core energy counter. It is 32-bit on legacy models;
  some newer supported models expose a 64-bit counter.

3. Energy consumed by Socket
   MSR_PACKAGE_ENERGY_STATUS/ C001_029B:
  Read-only cumulative socket energy counter, shared by cores in the socket.
  It is 32-bit on legacy models; some newer supported models expose a 64-bit
  counter.

These registers are updated every 1ms and cleared on
reset of the system.

Note: If SMT is enabled, Linux enumerates all threads as cpus.
Since, the energy status registers are accessed at core level,
reading those registers from the sibling threads would result
in duplicate values. Hence, energy counter entries are not
populated for the siblings.

Energy Calculation
------------------

Energy information (in Joules) is based on the multiplier,
1/2^ESU; where ESU is an unsigned integer read from
MSR_RAPL_POWER_UNIT register. Default value is 10000b,
indicating energy status unit is 15.3 micro-Joules increment.

Reported values are scaled as per the formula
`scaled value = ((1/2^ESU) * (Raw value) * 1000000UL)` in uJoules

Users calculate power for a given domain by calculating
	dEnergy/dTime for that domain.

Energy accumulation
--------------------------

For a 32-bit socket counter with an energy unit of 15.3 micro-Joules per
count, assuming 240W total across two equally loaded sockets (120W per socket),
the counter wraps in approximately:

`2^32 * 15.3e-6 / 120 = 547.6` seconds (about 9 minutes).

At 240W per socket, the same counter wraps in about 274 seconds. Actual wrap
time depends on the energy unit and socket power. Core-counter wrap time also
depends on the energy unit and per-core power; it should not be assumed to be
several days.

For supported CPUs with 32-bit counters, a kernel thread accumulates socket
energy and rotates through the core counters, adding each sampled delta to a
64-bit software counter. It starts during probe and stops when the driver is
removed. Each iteration samples every socket and one core; the core is changed
on the next iteration.

The accumulator interval is calculated during probe from the energy unit,
using a 250W power assumption to sample before a 32-bit counter can wrap. It
is not a fixed 100-second interval.

A socket and core energy read would return the current register
value added to the respective energy accumulator.

On supported AMD CPUs with 64-bit RAPL energy MSRs, the driver reads the
counters directly without software accumulation. Accumulation is used for the
specific AMD models listed in the driver as having 32-bit counters; this list
includes Ryzen and APU models as well as EPYC models.

Sysfs attributes
----------------

| Attribute       | Label             | Description                                                  |
| --------------- | ----------------- | ------------------------------------------------------------ |
| energy[X]_input | Ecore[X] energy   | X = [0] to [nr_cpus - 1] Measured input core energy          |
| energy[X]_input | Esocket[X] energy | X = [nr_cpus] to [nr_cpus + nr_socks - 1] Measured input socket energy |

Updates and Validation
----------------------

Recent updates improve compatibility with newer kernels and reliability of
energy readings. The driver now handles 64-bit RAPL counters without
truncation, checks MSR read failures, maps sensor channels using CPU topology,
and uses corrected counter-wrap and cache timing logic. The build, install,
and DKMS instructions have also been clarified.

These updates were developed with assistance from GitHub Copilot and tested
on hardware running Fedora 44, Linux kernel 7.2.15, and an AMD Ryzen 9 7950X.

Build and Install
-----------------

You need a C compiler, `make`, and the development headers for the kernel you
want to build against. Install the headers matching your running kernel. For
example:

| Distribution | Prerequisites |
| ------------ | ------------- |
| Fedora | `sudo dnf install gcc make kernel-devel-$(uname -r)` |
| Debian/Ubuntu | `sudo apt install build-essential linux-headers-$(uname -r)` |
| Arch Linux | `sudo pacman -S base-devel linux-headers` |

For Arch Linux, install the headers package matching your kernel variant (for
example, `linux-lts-headers` for `linux-lts`).

Run the following commands from the directory containing this repository.
By default, the Makefile builds against the running kernel using
`/lib/modules/$(uname -r)/build`.

Build the module:
```sh
make
```

Install it, refresh the module dependency map, and load it:
```sh
sudo make modules_install
sudo depmod -a
sudo modprobe zenergy
```

Confirm that the driver registered with hwmon:
```sh
grep -H '^zenergy$' /sys/class/hwmon/hwmon*/name
```
The matching `hwmonN` directory contains the `energy*_input` readings.

To build and install for a kernel other than the running one, set `KDIR` to
that kernel's build directory. These commands also derive the matching kernel
release for `depmod`:
```sh
KDIR=/path/to/kernel/build
KERNEL_RELEASE=$(make -s -C "$KDIR" kernelrelease)
make KDIR="$KDIR"
sudo make modules_install KDIR="$KDIR"
sudo depmod -a "$KERNEL_RELEASE"
```
Boot into that kernel before loading the module with `modprobe`.

To remove generated build files:
```sh
make clean
```

Loading
-------

If you built the module but did not install it, load it from the repository
directory with:
```sh
sudo insmod ./zenergy.ko
```

DKMS support
------------

DKMS requires the source in `/usr/src/zenergy-<version>`. The repository's
`dkms.conf` uses `@VERSION@` as a packaging placeholder, so substitute a
version when staging a checkout. From the repository directory, run:

```sh
VERSION=1.0
sudo install -d "/usr/src/zenergy-$VERSION"
sudo install -m 644 Makefile zenergy.c "/usr/src/zenergy-$VERSION/"
sed "s/@VERSION@/$VERSION/" dkms.conf | sudo tee "/usr/src/zenergy-$VERSION/dkms.conf" >/dev/null
sudo dkms add -m zenergy -v "$VERSION"
sudo dkms build -m zenergy -v "$VERSION" -k "$(uname -r)"
sudo dkms install --force -m zenergy -v "$VERSION" -k "$(uname -r)"
sudo modprobe zenergy
```

Use `--force` when replacing a module previously installed with
`make modules_install`; it lets DKMS replace that existing copy.

Install the matching kernel development headers before building. To target a
different installed kernel, replace `$(uname -r)` in the build and install
commands with that kernel's release; its headers must be installed as well.
After booting the target kernel, load the module with `sudo modprobe zenergy`.

Remove the DKMS registration with:
```sh
sudo dkms remove -m zenergy -v 1.0 --all
```
```