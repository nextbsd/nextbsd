# NextBSD

A BSD operating system derived from FreeBSD whose low-level system plumbing
uses open source components from Darwin: **`launchd` runs as PID 1**, services
are described by `.plist` files, hardware events flow through an in-kernel
IOKit registry over a Mach-IPC bus, and the network stack is configured by
the `IPConfiguration` daemon talking to `configd`. It is built on
the FreeBSD kernel — rebranded as NextBSD (`uname -s` is
`NextBSD`) — with ports of Darwin command-line tools making up the majority of
the userland.

If you just want to try it, jump to [Try it in 5 minutes](#try-it-in-5-minutes).
If you want the long technical answer to *what got ported and how*,
see [PORTING.md](PORTING.md).

## Heritage

This project is a re-implementation of the original
[NextBSD](https://github.com/NextBSD/NextBSD), not a fork of it. It reuses some of the
original's solutions, such as `libxpc`, and those carry enhancements from
[RavynOS](https://ravynos.com).  It is however built
entirely differently than those original solutions. 

## What's different from other BSDs?

| Other BSDs | NextBSD |
|---|---|
| `init(8)` is PID 1 | **`launchd(8)`** is PID 1 |
| Services configured via `rc(8)` — `rc.conf` + `rc.d/*` scripts | Services configured in **`.plist` files** under `/System/Library/LaunchDaemons/` |
| The base `syslogd(8)` | **Darwin's `syslogd`** (Apple System Logger / ASL) plus `notifyd` for the cross-process event bus |
| Device events via `devd`/`devmatch` (FreeBSD), `devpubd`/`drvctl` (NetBSD), `hotplugd` (OpenBSD) | **In-kernel IORegistry** (`/dev/ioregistry`) with an IOKit-shaped notification channel, browsed via Darwin's `libIOKit` / `ioreg` |
| Kernel modules via `kldload` (FreeBSD, `.ko`) or `modload`/`modctl` (NetBSD); OpenBSD dropped loadable modules | Kernel **extensions** loaded with **`kextload` / `kextstat`** (Darwin `kext_tools` driving `OSKext`); the `kld*` CLIs are retired (the `kld` syscalls remain) |
| A base DHCP client — `dhclient` (FreeBSD), `dhcpcd` (NetBSD), `dhcpleased` (OpenBSD) | **`ipconfigd`** (Darwin's IPConfiguration) handles DHCPv4 + ARP probing + lease renewal + publishes to `SCDynamicStore` |
| `mdnsd` (if installed) for Bonjour | **Darwin's `mDNSResponder`** with its full client API |
| Nothing equivalent | **`configd`** + `SCDynamicStore` — the system-wide key/value store every Darwin-source daemon expects |
| Nothing equivalent | **Mach IPC** in-kernel, plus `libsystem_kernel` / `libdispatch` / `libxpc` / `liblaunch` / `libCoreFoundation` in userland |
| `uname -s` names the upstream BSD; its own `*-version` tool | `uname -s` is **`NextBSD`**; **`nextbsd-version`** (see [Versioning](#versioning)) |

## Try it in 5 minutes

1. Grab the latest [continuous release][release]. Download
   `NextBSD-<ARCH>-<STAMP>.img.zip` (the raw disk image; there's also an
   `.iso.zip`). `<ARCH>` is `amd64` (x86-64) or `arm64` (AArch64) — both
   are built, boot-tested and published on every merge.
2. Unzip it (any platform's native tools work — macOS Finder,
   Windows Explorer, `unzip` on Linux/BSD):

   ```sh
   unzip NextBSD-amd64-*.img.zip
   ```

3. Boot it in [QEMU](https://www.qemu.org/) (easiest), or `dd` to a
   USB stick and boot on bare metal:

   ```sh
   # QEMU, UEFI boot, user-mode network (gives the guest 10.0.2.0/24)
   img=$(echo NextBSD-amd64-*.img)
   qemu-system-x86_64 \
     -accel kvm -cpu host -m 2048 \
     -bios /usr/share/OVMF/OVMF_CODE.fd \
     -drive file="$img",format=raw,if=virtio \
     -nic user,model=e1000 \
     -nographic
   ```

   On arm64 the machine is `virt`, the firmware is AAVMF/`QEMU_EFI.fd`,
   and virtio-net needs its PXE option ROM turned off (`romfile=`)
   because the arm64 QEMU packages don't ship one:

   ```sh
   img=$(echo NextBSD-arm64-*.img)
   qemu-system-aarch64 \
     -accel kvm -cpu host -m 2048 -machine virt \
     -bios /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
     -drive file="$img",format=raw,if=virtio \
     -netdev user,id=net0 -device virtio-net-pci,netdev=net0,romfile= \
     -nographic
   ```

   (`-accel kvm -cpu host` only works when the host is the same
   architecture as the image; drop it for emulation, which is slow.)

4. Wait ~10 seconds. You'll see launchd start its daemons, DHCP fire
   on `em0`, syslog come up, and the `login:` prompt land. Log in
   as `root` (no password, hit Enter).

[release]: https://github.com/nextbsd/nextbsd/releases/tag/continuous

## Networking just works

When the image boots, `ipconfigd` (Darwin's DHCPv4 client) DHCPs
the first Ethernet interface it finds. In QEMU's default user-mode
network you'll get `10.0.2.15/24`, gateway `10.0.2.2`, DNS via
`10.0.2.3`. On real hardware you'll get whatever your DHCP server
hands out.

Check it the Darwin way:

```sh
ipconfig getifaddr em0          # prints the IP address
ipconfig ifcount                # prints the count of interfaces
```

(`ipconfig(8)` here is Darwin's tool with the same name and CLI
shape, not FreeBSD's `ifconfig(8)`. Both exist on the image.)

If you want to see the routing table, default route, and so on,
standard `netstat -rn` works as you'd expect.

## Try some commands

These are the visible-to-users pieces of the Darwin-source stack
that's running. All work today on this image:

```sh
# launchctl — the Darwin service-control tool, manages launchd jobs.
launchctl list                       # show every job launchd knows about
launchctl list com.apple.syslogd     # plist for one specific job

# ipconfig — Darwin's IP-config CLI (NOT the same as ifconfig)
ipconfig getifaddr em0
ipconfig ifcount

# ioreg — Darwin's hardware-registry browser, over the in-kernel IORegistry
ioreg -l                             # the full registry tree
ioreg -c PCIDevice                   # filter by class
ioreg -n hostb0                      # find by name

# syslog — Darwin's log-query tool, served by syslogd's ASL store
syslog -F bsd                        # tail the system log in BSD format
syslog -k Sender ipconfigd           # filter by sender

# version identity — NextBSD's own
uname -a                             # ostype reads NextBSD
nextbsd-version                      # NextBSD 27.0.0 (<build>)
sw_vers                              # ProductName / ProductVersion / BuildVersion

# Standard tools are all still here
top
ps aux
pkg info
```

## What's running under the hood

```
+----------------+      +----------------+      +------------------+
|   launchctl    |----->|    launchd     |<-----| .plist files in  |
| /bin/launchctl |      |  /sbin/launchd |      | /System/Library/ |
+----------------+      | (PID 1)        |      |   LaunchDaemons/ |
                        +----------------+      +------------------+
                                |
                                | starts + monitors
                                v
   +----------------+----------------+---------------+---------------+
   | syslogd + ASL  |   notifyd      | mDNSResponder |   configd     |
   | /var/log/...   | event pub/sub  | Bonjour/DNS-SD| SCDynamicStore|
   +----------------+----------------+---------------+---------------+
                                                     |
                                                     | publishes to
                                                     v
                                            +------------------+
                                            |  ipconfigd       |
                                            |  (DHCP + lease)  |
                                            +------------------+
```

All of these talk to each other over **Mach IPC** (in-kernel), in userland via `libsystem_kernel.so`). All the daemons
that need a property-list store talk to `configd`'s `SCDynamicStore`
via `libSystemConfiguration.so`.

## Versioning

A NextBSD system is versioned `27.0.0 (<build>)`
([#536](https://github.com/nextbsd/nextbsd/issues/536)). `27.0.0` is the
kernel's release (`uname -r`) and `<build>` is the nextbsd-pkg run that built
the packages; every NextBSD package is versioned `27.0.0.<build>`.

The installed `NextBSD-everything` package owns Darwin's
`/System/Library/CoreServices/SystemVersion.plist` (`ProductName`,
`ProductVersion`, `ProductBuildVersion`), and everything that shows the version
reads it from the installed system, so a fresh image and a `pkg upgrade`d one
agree:

```sh
sw_vers                  # ProductName: NextBSD, ProductVersion: 27.0.0, BuildVersion: <build>
sw_vers -productVersion  # 27.0.0
nextbsd-version          # NextBSD 27.0.0 (<build>)
pkg info NextBSD-everything   # NextBSD-everything-27.0.0.<build>
cat /etc/os-release      # PRETTY_NAME="NextBSD 27.0.0 (<build>)", VERSION_ID, BUILD_ID, ...
```

`/etc/os-release`, which the login banner reads, is generated from the plist:
by `build.sh` for a fresh image, and on an installed system by
`NextBSD-everything`'s post-install script, which pkg runs on every upgrade of
the package. The image and ISO names carry the same pair:
`NextBSD-<arch>-27.0.0-<build>.img.zip`. Images built from packages that predate
the plist fall back to the build's UTC timestamp (`YYYYMMDD-HHMMSS`) for all of
these.

## How it's built

NextBSD is assembled from a small chain of repositories, each publishing a
rolling `continuous` release that the next stage ingests:

- **[nextbsd-kernel](https://github.com/nextbsd/nextbsd-kernel)** —
  the NextBSD kernel, built as a patch + overlay set on top of FreeBSD
  (the FreeBSD source tree is never forked in place).
- **[nextbsd-freebsd-compat](https://github.com/nextbsd/nextbsd-freebsd-compat)** —
  the curated FreeBSD-source base userland, built from a srclist.
- **[nextbsd-kernel-extensions-graphics](https://github.com/nextbsd/nextbsd-kernel-extensions-graphics)** —
  graphics kexts (drm-kmod), plus IntelWiFi and IntelEthernet for now
  (KPI-matched to the kernel).
- **[nextbsd-kernel-extensions-nvidia](https://github.com/nextbsd/nextbsd-kernel-extensions-nvidia)** —
  the NVIDIA kexts and userland (amd64).
- **this repo** — assembles the bootable image/ISO: it lays the from-source
  base, builds the Darwin-source userland and the Mach/launchd stack on top,
  ingests the kernel + driver kexts, and packages a GPT disk image (BIOS +
  UEFI, read-write UFS root) plus a cd9660 ISO.

It all runs on GitHub Actions' **Linux (Ubuntu) runners** — there's no FreeBSD
hardware in the loop. The kernel and the from-source base are **cross-compiled
on Linux** with a FreeBSD cross-toolchain (clang `-target …-freebsd`, in a
prebuilt toolchain container), so those stages never need a FreeBSD host. The
final image assembly (`build.sh`) then runs inside a **FreeBSD VM**
(`vmactions/freebsd-vm`) on that same Linux runner — it needs a real FreeBSD
userland to run `makefs`/`mkimg` and lay the image down — which also boot-tests
the result and refreshes the `continuous` release on a successful `main` build.
Because the kernel and base arrive as published artifacts, `build.sh` is a CI
orchestration step, not a standalone local-build command.

## Releases

Each successful `main` build publishes versioned assets to the
[continuous release][release] — the zip-wrapped GPT disk image
(`NextBSD-<arch>-<stamp>.img.zip`) and ISO (`NextBSD-<arch>-<stamp>.iso.zip`),
each with a matching `.sha256`. The latest is always at that tag.

## Going deeper

- **[PORTING.md](PORTING.md)** — full technical history of
  what's been ported and why. Phase-by-phase, with per-component
  rationale.
- **[Issues](https://github.com/nextbsd/nextbsd/issues)**
  — open work items, scoping questions, planned ports.

## License

[BSD-2-Clause](LICENSE), with per-component Apache 2.0 / LGPL / MIT /
OSF / CMU headers preserved on imported files — see
[NOTICE](NOTICE).
