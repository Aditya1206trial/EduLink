# EduLink

A Linux PCI driver for QEMU's educational `edu` device
(vendor:device `1234:11e8`), built in phases. The `edu` device itself is
part of QEMU (`-device edu`); this repository provides only the driver and
its user-space test.

> **Status: Phase 3, validated in a QEMU guest.**
> Built and loaded in an Ubuntu 24.04 guest (kernel 6.8.0-147-generic,
> x86_64) under QEMU 11.1.0 with TCG. The full `test_edu -n 10000` run
> passed, including factorial IRQs and DMA round trips, and module
> unload/reload and PCI sysfs remove/rescan worked. Not tested on physical
> hardware or arm64. See [Validation so far](#validation-so-far) for exactly
> what was and was not covered.

## Phase 3 scope

Phase 3 adds one ioctl, `EDU_IOC_DMA`: a round trip through the device's
DMA engine. Liveness and factorial are unchanged from phase 2.

Implemented:

- PCI probe for `1234:11e8`, BAR0 claimed and mapped as MMIO.
- Identification register check (low 16 bits must be `0x00ed`) and a
  liveness round-trip at probe time; probe fails otherwise.
- Shared legacy INTx interrupt (`request_irq(pdev->irq, ..., IRQF_SHARED)`)
  and factorial-completion interrupt enabled (status bit `0x80` set).
  MSI is not used. See [Interrupt handling](#interrupt-handling).
- Misc character device `/dev/edu0`, mode `0600`. Only one edu device is
  supported; probing a second one fails with `EBUSY`.
- `EDU_IOC_LIVENESS`: writes a value to the liveness register and returns
  the raw read-back (a healthy device returns the bitwise inverse).
- `EDU_IOC_FACTORIAL`: computes `n!` on the device for `0 <= n <= 12`.
  The ioctl sleeps until the factorial interrupt wakes it (1 s timeout,
  interruptible by signals). No polling.
- `EDU_IOC_DMA`: copies 1..4096 bytes RAM -> device buffer -> RAM using
  one coherent DMA buffer and a 28-bit DMA mask, and returns the bytes
  read back. Interrupt-driven, 1 s timeout per wait, interruptible. See
  [DMA](#dma).
- Hot-unplug / open-file lifetime handling (see below).
- User-space test `test_edu` with functional checks and stress runs.

Not implemented (do not rely on them): MSI, streaming DMA
(`dma_map_single()` of user pages), a configurable DMA mask, the interrupt
raise register (`0x60`), `read`/`write`/`mmap`, more than one device,
32-bit kernels (see [DMA](#dma)).

## Register map used

Confirmed against <https://www.qemu.org/docs/master/specs/edu.html>.
BAR0 is a 1 MB MMIO region; offsets below `0x80` allow 4-byte access only,
offsets from `0x80` allow 4- or 8-byte access.

| Offset | Access | Meaning                                                   | Used |
|--------|--------|-----------------------------------------------------------|------|
| `0x00` | RO     | Identification, `0xRRrr00ed` (RR major, rr minor)         | yes  |
| `0x04` | RW     | Liveness: reads back the bitwise inverse of the write     | yes  |
| `0x08` | RW     | Factorial: write n, read n! once not busy                 | yes  |
| `0x20` | RW     | Status: `0x01` busy (RO), `0x80` IRQ on factorial done    | yes (`0x80` set) |
| `0x24` | RO     | Interrupt status; `0x01` factorial done, `0x100` DMA done | yes (handler) |
| `0x60` | WO     | Interrupt raise                                           | no   |
| `0x64` | WO     | Interrupt acknowledge (clears the written bits)           | yes (handler) |
| `0x80` | RW     | DMA source address                                        | yes, 64-bit |
| `0x88` | RW     | DMA destination address                                   | yes, 64-bit |
| `0x90` | RW     | DMA transfer count                                        | yes, 64-bit |
| `0x98` | RW     | DMA command: `0x01` start, `0x02` direction (0 RAM -> device, 1 device -> RAM), `0x04` raise IRQ `0x100` when done | yes, 64-bit |

The device's DMA buffer is 4096 bytes at device address `0x40000`. By
default the device handles only 28-bit (256 MiB) DMA addresses
(`-device edu,dma_mask=...` changes that; this driver assumes the default).

## ioctl ABI (`edu_ioctl.h`)

`EDU_IOC_LIVENESS` and `EDU_IOC_FACTORIAL` take a pointer to a `__u32` used
for input and output. `EDU_IOC_DMA` takes a pointer to:

```c
struct edu_dma {
	__u32 len;                       /* 1..EDU_DMA_MAX_LEN (4096) */
	__u8 data[EDU_DMA_MAX_LEN];
};                                       /* 4100 bytes, same on 32/64-bit */
```

| Command             | Number       | In          | Out              | Errors |
|---------------------|--------------|-------------|------------------|--------|
| `EDU_IOC_LIVENESS`  | `0xc0046501` | any value   | raw read-back    | `EFAULT`, `ENODEV`, `EINTR`/restart |
| `EDU_IOC_FACTORIAL` | `0xc0046502` | `n` in 0..12 | `n!`            | `ERANGE` (n > 12), `ETIMEDOUT`, `ENODEV`, `EFAULT`, `EINTR`/restart |
| `EDU_IOC_DMA`       | `0xd0046503` | `len`, `data[0..len-1]` | `data[0..len-1]` read back from the device | `EINVAL` (len 0 or > 4096), `ETIMEDOUT`, `ENODEV`, `EFAULT`, `EINTR`/restart |

`EDU_IOC_DMA` writes only `data[0..len-1]`, and only on success. `len` and
the rest of `data` are not written.

Unknown commands return `ENOTTY`. The device's factorial register is 32
bits wide and wraps silently, so inputs above 12 are rejected rather than
returning a truncated result. Ioctls on one device are serialized by one
mutex. 32-bit callers on a 64-bit kernel go through `compat_ptr_ioctl`.

## DMA

- **Probe.** `dma_set_mask_and_coherent(DMA_BIT_MASK(28))`, then probe
  fails with `EBUSY` if the DMA command register still has START set (a
  transfer from an earlier owner). Then one `dma_alloc_coherent()` buffer
  of 4096 bytes is allocated for the device's lifetime, and
  `pci_set_master()` is called after the IRQ is set up.
- **Round trip**, under the device mutex:
  1. Wait until START (`0x98` bit `0x01`) is clear. A transfer abandoned by
     an earlier timed-out or interrupted ioctl may still be running, and
     the device ignores DMA register writes while START is set.
  2. `copy_from_user()` `len` bytes into the coherent buffer.
  3. Leg 1: `0x80` = buffer bus address, `0x88` = `0x40000`, `0x90` = `len`,
     `0x98` = `0x05` (start, RAM -> device, IRQ). Wait.
  4. `memset()` the first `len` bytes of the buffer to 0, so the result can
     only come from the device.
  5. Leg 2: `0x80` = `0x40000`, `0x88` = buffer bus address, `0x90` = `len`,
     `0x98` = `0x07` (start, device -> RAM, IRQ). Wait.
  6. `copy_to_user()` `len` bytes.
- **Register access.** All four DMA registers use `readq()`/`writeq()`,
  which must be single 8-byte accesses. Kernel target: 64-bit only,
  x86_64 and arm64 (both have native `readq()`/`writeq()`; arm64 checked
  in `arch/arm64/include/asm/io.h`: one `ldr`/`str`). A 32-bit kernel
  build stops with `#error`. Why: QEMU's `edu_mmio_read()`/`write()`
  decode only offsets `0x80`, `0x88`, `0x90`, `0x98`. A split access
  (for example `<linux/io-64-nonatomic-lo-hi.h>`) also touches `0x84`,
  `0x8c`, ...; those reads return all ones and those writes are dropped.
- **Completion.** Each wait is `wait_event_interruptible_timeout()` on a
  DMA wait queue, timeout 1 s. The DMA IRQ (`0x100`) only wakes it. The
  START bit decides. QEMU clears START and raises the IRQ in one step, so
  the late IRQ of an abandoned transfer can only reach a waiter of a later
  transfer, which then still sees START set and sleeps again.
- **Memory ordering.** The buffer is coherent, so no sync calls. `writeq()`
  of the command register is ordered after the CPU's earlier stores to the
  buffer (`copy_from_user()`, `memset()`). The `readq()` that sees START
  clear is ordered before the CPU's later reads of the buffer. Both are the
  documented ordering guarantees of non-relaxed MMIO accessors.
- **Timeout or signal.** The ioctl returns `ETIMEDOUT`/`EINTR`, and the
  transfer keeps running into the driver's own buffer, which stays
  allocated. The next DMA ioctl waits for it first. User memory is never
  the DMA target.
- **Removal.** EDU has **no DMA abort**: a started transfer always runs to
  the end (QEMU runs it on a timer 100 ms of virtual time after start).
  Under the mutex, `remove()` calls `pci_clear_master()` (in QEMU this
  disables the device's bus-master address space, so a pending transfer
  no longer reaches RAM) and then waits up to 1 s for START to clear, still
  woken by the DMA IRQ. Only then is the IRQ freed. The buffer is freed
  only if START cleared, or the device reads back all ones (gone). If it
  is still set after 1 s, the driver logs `DMA did not stop; leaking its
  buffer` and does **not** free the 4 KiB buffer. That leak is the
  explicit limit. A later probe then fails with `EBUSY` while START stays
  set.
- **Failed probe.** No transfer is started during probe, so the buffer is
  freed directly. If `misc_register()` fails after `pci_set_master()`,
  bus master is cleared first.

## Interrupt handling

- **Handler.** Reads `0x24`. Zero means another device on the shared line
  raised it: return `IRQ_NONE`. `0xffffffff` means the device is gone:
  also `IRQ_NONE`, no write. Otherwise write the same value to `0x64`
  (acks every pending bit so INTx drops). If bit `0x01` is set, wake the
  factorial wait queue. If bit `0x100` is set, wake the DMA wait queue.
  Returns `IRQ_HANDLED`.
- **Factorial ioctl.** Under the device mutex: wait until not busy, write
  `n` to `0x08`, wait until not busy, read `0x08`. Each wait is
  `wait_event_interruptible_timeout()` on the busy bit (`0x20` bit `0x01`),
  timeout 1 s. The busy bit is read once before sleeping and again only
  after an interrupt wakes the waiter. There is no timed polling loop.
- **Late IRQ after timeout or signal.** The interrupt only wakes the
  waiter. The busy bit decides whether the factorial is done. QEMU clears
  busy first and raises the IRQ afterwards. So the IRQ of an abandoned
  factorial can arrive after the next factorial started. That waiter wakes,
  sees busy still set and sleeps again. It cannot return the wrong result.
  If an abandoned factorial is still running, the next ioctl first sleeps
  until it finishes. The device ignores factorial writes while busy. DMA
  uses the same rule with the START bit (see [DMA](#dma)).
- **Setup order (probe).** Map BAR0 and check identification/liveness, set
  the DMA mask and allocate the DMA buffer, set up the wait queues,
  `request_irq()`, then `pci_intx(pdev, 1)`, then set `0x80`, then
  `pci_set_master()`. `pci_intx(pdev, 1)` clears
  `PCI_COMMAND_INTX_DISABLE`, which an earlier owner (or this driver's own
  teardown) may have left set. It runs only after the handler is
  registered: if bits are still pending, QEMU asserts INTx as soon as it is
  unmasked, and the handler acks them. Probe fails with `ENODEV` if the
  device has no INTx assigned (`pdev->irq == 0`).
- **Teardown (remove and failed probe after `request_irq`).** Clear `0x80`,
  mask INTx with `pci_intx(pdev, 0)` (sets `PCI_COMMAND_INTX_DISABLE`),
  `free_irq()`, then ack anything still pending. The mask comes before
  `free_irq()` because a factorial abandoned just before teardown may
  already have read `0x80` as set; QEMU checks it before taking its lock.
  Its interrupt then either reaches the still-registered handler, or is
  latched in the device while INTx is masked. It never asserts the shared
  line with no handler. A bit latched after the final ack stays pending
  until the next probe unmasks INTx. Teardown runs before the MMIO pointer
  is cleared and the BAR unmapped, because the handler reads it. In remove
  it runs under the device mutex, after any in-flight ioctl and after DMA
  is quiesced. INTx is left masked after remove.
- `pci_intx()` semantics checked in the kernel's `drivers/pci/pci.c`
  (master): a plain read-modify-write of `PCI_COMMAND`, no-op if
  unchanged. Older kernels also registered a devres restore action, but
  only for managed devices (`pcim_enable_device()`). This driver uses `pci_enable_device_mem()`.
  QEMU (`hw/pci/pci.c`) records INTx level while the disable bit is set
  and re-asserts it when the bit is cleared.

## Lifetime and hot-unplug

- `open()` takes a reference on the per-device state. `misc_open()` calls
  the driver's `open` while holding `misc_mtx`, which `misc_deregister()`
  also takes, so open cannot race with removal.
- `remove()` deregisters the misc device, sets a `removed` flag and wakes
  the factorial and DMA waiters. Both wait conditions check the flag
  first, without MMIO, so a waiter returns `ENODEV` at once. This covers
  orderly removal (`device_del`), where the device can still be present
  and busy. An ioctl that starts after the flag is set also returns
  `ENODEV`. `remove()` then takes the device mutex (waiting for the
  in-flight ioctl to return), quiesces DMA (see [DMA](#dma)), tears down
  the IRQ, clears the MMIO pointer, unmaps the BAR, frees the DMA buffer
  (unless DMA did not stop) and disables the PCI device. The BAR stays
  mapped while any ioctl holds the mutex. Later ioctls on still-open files
  return `ENODEV` and never touch the DMA buffer. The last `close()` frees
  the state.
- A status or DMA command read of all ones (what a surprise-removed device
  returns) is also treated as `ENODEV`.
- `.owner = THIS_MODULE` stops `rmmod` while a file is open.

## Setup

### 1. Guest VM with the edu device

The validated run used QEMU 11.1.0 on a Windows 11 host with an Ubuntu
24.04 (noble) cloud image (SHA-256 checked against Ubuntu's published
sums) and a cloud-init `CIDATA` seed ISO built with `pycdlib`.

The steps below are a reproducible recipe for a Linux host, not the exact
host commands of the validated run. The machine options (`-machine`,
`-accel`, `-cpu`, `-m`, `-smp`, `-device edu`) and the host port forward
bound to `127.0.0.1` match it. Needs `qemu-system-x86`, `qemu-utils`,
`cloud-image-utils` (for `cloud-localds`) and an SSH key pair.

```sh
# Ubuntu 24.04 cloud image, checked against Ubuntu's SHA256SUMS
wget https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img
wget https://cloud-images.ubuntu.com/noble/current/SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS

# Copy-on-write overlay with room for headers and the build
qemu-img create -f qcow2 -F qcow2 -b noble-server-cloudimg-amd64.img guest.qcow2 20G

# cloud-init seed: inject your own public key for the default user "ubuntu"
cat > user-data <<EOF
#cloud-config
ssh_authorized_keys:
  - $(cat ~/.ssh/id_ed25519.pub)
EOF
cloud-localds seed.img user-data

qemu-system-x86_64 \
    -machine pc -accel tcg,thread=multi -cpu max -m 2048 -smp 2 \
    -drive file=guest.qcow2,if=virtio \
    -drive file=seed.img,if=virtio,format=raw \
    -nic user,hostfwd=tcp:127.0.0.1:2222-:22 \
    -device edu \
    -nographic
```

From another host terminal, once the guest has booted, copy the sources in
and log in:

```sh
scp -P 2222 -r EduLink ubuntu@127.0.0.1:
ssh -p 2222 ubuntu@127.0.0.1
```

`-nographic` puts the guest console on the terminal; quit QEMU with
`Ctrl-a x`. On a Linux host, `-accel kvm -cpu host` is optional and
faster; it was not used for the validated run. `-device edu` is required;
check `qemu-system-x86_64 -device help | grep edu`. Do not set `dma_mask`;
the driver assumes the 28-bit default. The `pc` machine is used because its
ACPI PCI hotplug supports the optional `device_del` check below.

### 2. Inside the guest

```sh
lspci -nn | grep 1234:11e8          # device must be visible
sudo apt install build-essential linux-headers-$(uname -r)   # Debian/Ubuntu
# Fedora: sudo dnf install gcc make kernel-devel-$(uname -r)

cd EduLink
make                                 # builds edu_drv.ko and test_edu
sudo insmod edu_drv.ko
sudo dmesg | tail                    # "edu device version 1.0" and "registered /dev/edu0"
ls -l /dev/edu0                      # crw------- (mode 0600)
sudo ./test_edu -n 10000             # default count 2000; device defaults to /dev/edu0
grep edulink /proc/interrupts        # shows the IRQ line edulink registered on
sudo dmesg | grep -i -E 'edulink|edu:|nobody cared|irq'   # expect no warnings
sudo rmmod edu_drv
```

The legacy INTx line is shared, so `/proc/interrupts` counts every
interrupt on it, not only edu interrupts. In the validated guest it showed
`11: 0 23194 IO-APIC ... ens3, edulink`: IRQ 11 shared with the `ens3`
NIC. That line confirms the handler is registered; its count does not tell
how many interrupts the edu device raised.

QEMU logs DMA range errors (`EDU: DMA range ... out of bounds`, `EDU:
clamping DMA ...`) only with `-d guest_errors`. Adding it to the QEMU
command line is a useful extra check that no transfer was misprogrammed.

Targets: `make module`, `make test`, `make clean`. Override the kernel tree
with `make KDIR=/path/to/build`. Secure Boot guests may refuse unsigned
modules; disable it in the guest firmware or sign the module.

### 3. `test_edu` checks and output

`test_edu [-n count] [device]`. After the functional checks it runs:

1. Factorial stress, plain: `count` factorials (default 2000, max
   100000000), `n` cycles through 0..12 and each result is checked.
2. Factorial stress with signals: the same, while a 200 us `SIGALRM` timer
   (no `SA_RESTART`) interrupts ioctls. An `EINTR` is retried. A retry runs
   right after an abandoned factorial, so it can receive that factorial's
   late IRQ, and must then still return its own correct result. Whether a
   given retry actually saw a late IRQ is not observed. Fails if any result
   is wrong, any error other than `EINTR` occurs, or `EINTR` exceeds
   20 x `count` (livelock guard). Prints a `NOTE` if no ioctl was
   interrupted, because then the late-IRQ path was not exercised.
3. DMA stress with signals: 50 round trips (fixed), lengths
   `1 + (i * 509) % 4096`. Attempts cycle through no timer and a one-shot
   `SIGALRM` after 5, 50, 150 or 250 ms, so every fifth attempt is not
   interrupted. The delays aim at before the transfer, leg 1, leg 2 and
   after it, but the timer is host-side real time and each leg ends 100 ms
   of QEMU virtual time after it starts; which phase a signal actually hits
   is not checked. `EINTR` is retried with fresh data. Each success must
   match byte for byte, sentinel bytes past `len` included. Prints a
   `NOTE` if no ioctl was interrupted.

The functional DMA checks: lengths 1, 2, 255, 256, 257, 1000, 4095 and
4096 with varied byte patterns must round-trip exactly. Payload bytes are
never 0, because the driver zeroes its buffer before the read-back leg; a
byte the device did not write then fails the check. Bytes past `len`
hold a `0xa5` sentinel that must come back unchanged. Lengths 0, 4097 and
`0xffffffff` must fail with `EINVAL`. A bad pointer must fail with `EFAULT`.

Run time has not been measured. From QEMU's source, each DMA leg completes
100 ms of virtual time after it starts, so each round trip takes at least
200 ms of guest time. The `EINTR` counts vary from run to run.

Actual output of `sudo ./test_edu -n 10000` in the validated guest (exit
status 0):

```
PASS: liveness 0x00000000 -> 0xffffffff
PASS: liveness 0xffffffff -> 0x00000000
PASS: liveness 0x12345678 -> 0xedcba987
PASS: factorial 0 -> 1 (want 1)
PASS: factorial 10 -> 3628800 (want 3628800)
PASS: factorial 12 -> 479001600 (want 479001600)
PASS: factorial 13 rejected with ERANGE
PASS: unknown ioctl rejected with ENOTTY
PASS: bad user pointer rejected with EFAULT
PASS: dma len 1 round trip
PASS: dma len 2 round trip
PASS: dma len 255 round trip
PASS: dma len 256 round trip
PASS: dma len 257 round trip
PASS: dma len 1000 round trip
PASS: dma len 4095 round trip
PASS: dma len 4096 round trip
PASS: dma len 0 rejected with EINVAL
PASS: dma len 4097 rejected with EINVAL
PASS: dma len 4294967295 rejected with EINVAL
PASS: dma bad user pointer rejected with EFAULT
PASS: stress: 10000/10000 factorials correct, 0 EINTR
PASS: stress with signals: 10000/10000 factorials correct, 28 EINTR
PASS: dma stress with signals: 50/50 round trips correct, 196 EINTR
OK: 0 failure(s)
```

Exit status is 0 only when every check passes.

### 4. Removal checks

**PCI sysfs remove (run).** With the module loaded, the validated guest ran
this sequence:

```sh
echo 1 | sudo tee /sys/bus/pci/devices/0000:00:04.0/remove
sudo rmmod edu_drv
echo 1 | sudo tee /sys/bus/pci/rescan
lspci -nn | grep 1234:11e8                   # device detected again
sudo insmod edu_drv.ko                       # driver loaded manually
ls -l /dev/edu0
```

Use your device's address from `lspci`. Each step succeeded and `/dev/edu0`
was back after the manual `insmod`. Automatic rebind on rescan (rescan with
the module still loaded) was not tested. Separate `rmmod`/`insmod` cycles
also worked.

**Deliberate negative test: sysfs remove during a running test (run).**
This is expected to make `test_edu` fail; it checks that the failure is
clean, not that the test passes. The validated guest ran
`sudo stdbuf -oL ./test_edu -n 1` (line-buffered so progress is visible),
waited until it reached the DMA stress, then issued the sysfs remove
above. Result:

```
FAIL: dma stress #24 len 4025: No such device
```

`test_edu` exited with status 1 (`ENODEV`, as intended), with no hang and
no new kernel warning in `dmesg`, and `sudo rmmod edu_drv` then succeeded.
This is one run with removal at one point in time; it does not prove the
removal path is race-free at every point.

**QEMU `device_del` (not run).** Start QEMU with `-device edu,id=edu0` and
`-monitor stdio`. In the guest, hold `/dev/edu0` open (for example
`sudo sh -c 'exec 3</dev/edu0; sleep 600'`), then in the QEMU monitor run
`device_del edu0`. Intended outcome: the node disappears, no oops in
`dmesg`, and the module can be removed once the holder exits. Repeat with
`sudo ./test_edu -n 1000000` running instead of `sleep`; it should end
with `ENODEV` failures, not a hang or an oops.

## Validation so far

### Guest run (QEMU, 2026-10-27)

Environment: Windows 11 host, QEMU 11.1.0, `-machine pc -accel tcg,thread=multi
-cpu max -m 2048 -smp 2 -device edu`. Guest: Ubuntu 24.04 (noble) cloud
image, SHA-256 verified, kernel `6.8.0-147-generic` (x86_64) with matching
headers, GCC and Make.

- `lspci -nn`: `00:04.0 Unclassified device [00ff]: Device [1234:11e8] (rev 10)`.
- `make` exited 0 and produced `edu_drv.ko` and `test_edu` against the real
  kernel build tree.
- `sudo insmod edu_drv.ko` exited 0. `dmesg`:
  `edulink 0000:00:04.0: edu device version 1.0` and
  `registered /dev/edu0`. Node mode `crw-------`.
  `dma_set_mask_and_coherent()` with the 28-bit mask therefore succeeded on
  this kernel (probe fails otherwise), with 2 GiB of guest RAM.
- `sudo ./test_edu -n 10000` exited 0 with `OK: 0 failure(s)` (full output
  in [section 3](#3-test_edu-checks-and-output)). This exercised liveness,
  factorial completion by INTx interrupt, the error paths (`ERANGE`,
  `ENOTTY`, `EFAULT`, `EINVAL`), coherent-buffer DMA round trips at all
  tested lengths, and the signal paths: 28 `EINTR` in factorial stress and
  196 in DMA stress, all retried to correct results.
- `sudo rmmod edu_drv` succeeded; a further reload/unload cycle succeeded.
- PCI sysfs remove, `rmmod`, rescan (device `1234:11e8` detected again),
  then manual `insmod` restored `/dev/edu0`. Automatic rebind was not
  tested.
- `grep edulink /proc/interrupts` showed
  `11: 0 23194 IO-APIC ... ens3, edulink`: the handler is registered on
  IRQ 11, shared with the `ens3` NIC. No per-device interrupt count was
  derived from it.
- Deliberate negative test (expected to fail, see
  [section 4](#4-removal-checks)): sysfs remove during the DMA stress of
  `sudo stdbuf -oL ./test_edu -n 1` made it exit 1 with
  `FAIL: dma stress #24 len 4025: No such device`.
  No hang, no new kernel warning, and `rmmod` succeeded afterwards.

### Static checks before the guest run (Windows 11, 2026-10-27)

These were done without a Linux kernel tree or QEMU; the guest run above
supersedes them where they overlap.

- DMA register offsets, command bits, IRQ bit `0x100`, buffer address
  `0x40000`, size 4096 and the 28-bit default mask checked in the spec page
  above and in QEMU's `hw/misc/edu.c` (master): the transfer runs on a
  100 ms virtual-clock timer; START is cleared and then `0x100` raised (if
  `0x04` is set) in the same callback; writes to `0x80`..`0x98` are ignored
  while START is set; a write to `0x98` without START is ignored, so there
  is no abort; out-of-range transfers are skipped with a log message.
  QEMU's `hw/pci/pci.c` (master) checked: clearing `PCI_COMMAND_MASTER`
  disables the device's bus-master address space.
- Interrupt behaviour checked in QEMU's `hw/misc/edu.c` (master): `0x24`
  returns `irq_status`; a write to `0x64` clears the written bits and drops
  INTx once none remain (MSI off); the factorial thread clears busy, then
  raises bit `0x01` under the BQL only if `0x80` is set; writes to `0x08`
  while busy are ignored; there is no reset handler that clears
  `irq_status`.
- `struct edu_dma` size 4100, `data` offset 4 and the three ioctl numbers
  checked with `_Static_assert` against the UAPI `_IOC` macros for
  `x86_64-linux-gnu`, `x86-linux-gnu` and `aarch64-linux-gnu`, so the
  32/64-bit layouts match and `compat_ptr_ioctl` needs no translation.
- `test/test_edu.c` compiled with `zig cc` 0.16.0 using
  `-std=c11 -O2 -Wall -Wextra -Werror -I.` for `x86_64-linux-gnu`,
  `x86_64-linux-musl`, `aarch64-linux-gnu` and `x86-linux-gnu`: no
  warnings. A host harness ran `dma_fill()` over seeds 0..63: every
  payload byte nonzero, every byte past `len` `0xa5`.
- `edu_drv.c` compiled against hand-written stub headers for `x86_64`,
  `x86` and `aarch64` (syntax and stub-level types only). Since then the
  driver is 64-bit only (see [DMA](#dma)); the `x86` build stops at
  `#error`.

### Not covered

- Physical hardware (the `edu` device exists only in QEMU), KVM, and any
  architecture other than x86_64. The arm64 `readq()`/`writeq()` claim in
  [DMA](#dma) rests on reading kernel source only.
- Debugging under GDB, MSI (not implemented), streaming DMA (not
  implemented), concurrent access from several processes or threads, and
  QEMU `device_del` hot-unplug.
- The exact hot-unplug races described in
  [Lifetime and hot-unplug](#lifetime-and-hot-unplug) and [DMA](#dma)
  (removal timed against a specific DMA leg, an in-flight late IRQ, the
  "DMA did not stop" leak path). Only the single sysfs-remove runs above
  were done.
- Edu-specific interrupt counts (the `/proc/interrupts` line is shared
  with `ens3`), `-d guest_errors` QEMU logging and run time were not
  recorded.
- Which DMA phase each stress signal hit, and whether any retry received
  a late IRQ from an abandoned operation.
- The Linux-host recipe in [Setup](#1-guest-vm-with-the-edu-device) was
  not run as written. The validated host was Windows, with an equivalent
  `CIDATA` seed ISO built by `pycdlib`.

## Files

| Path               | Purpose                              |
|--------------------|--------------------------------------|
| `edu_drv.c`        | Kernel module                        |
| `edu_ioctl.h`      | Shared ioctl ABI (kernel and user)   |
| `test/test_edu.c`  | User-space functional + stress test  |
| `Makefile`         | Kbuild + test build                  |
