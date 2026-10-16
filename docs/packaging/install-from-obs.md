# Installing mhVTL from the OBS package repositories

> **Draft, not published.** These repositories are a personal snapshot build
> (`home:ciriarte:mhvtl` on the openSUSE Build Service), not packages the
> mhVTL project supports. Whether the project wants to adopt them is still
> waiting on the maintainer
> ([markh794/mhvtl#270](https://github.com/markh794/mhvtl/issues/270)). Until
> then, treat them as unofficial test builds.

Prebuilt packages for current openSUSE, RHEL-compatible, Debian and Ubuntu
releases are built from upstream `master` on the openSUSE Build Service:

- Project: <https://build.opensuse.org/project/show/home:ciriarte:mhvtl>
- Repositories: <https://download.opensuse.org/repositories/home:/ciriarte:/mhvtl/>

Each distribution gets:

| Package | Contents |
|---|---|
| `mhvtl-utils` (RPM) / `mhvtl` (deb) | `vtltape`, `vtllibrary`, the command line tools, systemd units, sample configuration |
| `mhvtl-dkms` | source of the `mhvtl` kernel module, built by DKMS for each installed kernel |
| `mhvtl-kmp-default` (openSUSE only) | the kernel module prebuilt for the distribution kernel (KMP) |

On openSUSE use the KMP. Elsewhere the module comes from DKMS.

## Supported releases

| Distribution | Repository path | Kernel module |
|---|---|---|
| openSUSE Tumbleweed | `openSUSE_Tumbleweed` | KMP |
| openSUSE Slowroll | `openSUSE_Slowroll` | KMP |
| openSUSE Leap 16.0 | `openSUSE_Leap_16.0` | KMP |
| Rocky Linux 9, RHEL 9 | `RockyLinux_9` | DKMS |
| Rocky Linux 10, RHEL 10 | `RockyLinux_10` | DKMS |
| AlmaLinux 9 | `AlmaLinux_9` | DKMS |
| AlmaLinux 10 | `AlmaLinux_10` | DKMS |
| Debian 12 | `Debian_12` | DKMS (see below) |
| Debian 13 | `Debian_13` | DKMS |
| Ubuntu 24.04 LTS | `xUbuntu_24.04` | DKMS |
| Ubuntu 26.04 LTS | `xUbuntu_26.04` | DKMS |
| Proxmox VE 9 | `Debian_13` | DKMS |
| Proxmox VE 8 | `Debian_12` | DKMS (see below) |

**Debian 12 / Proxmox VE 8:** the `Debian_12` repository does not publish yet. Bookworm's debhelper only
looks for unit files in `/lib/systemd/system`, while mhVTL installs them in
`/usr/lib/systemd/system`, so the build fails. The fix (take the directory from
`systemd.pc`) is on the `packaging/debian12-units` branch of
<https://github.com/ciroiriarte/mhvtl> and not yet proposed upstream.

All x86_64. RHEL 10 and its rebuilds need an x86-64-v3 CPU (Intel Haswell,
AMD Zen or newer); that is a requirement of the distribution, not of mhVTL.

## Install

Run the commands as root.

### openSUSE Tumbleweed, Leap 16.0

```sh
zypper addrepo --refresh \
    https://download.opensuse.org/repositories/home:/ciriarte:/mhvtl/openSUSE_Tumbleweed/ mhvtl
zypper --gpg-auto-import-keys refresh mhvtl
zypper install mhvtl-utils mhvtl-kmp-default
```

For Leap 16.0 use `openSUSE_Leap_16.0` in the URL.

The KMP needs the full `kernel-default`. Minimal and cloud images often ship
`kernel-default-base` instead; replace it and reboot into the new kernel
before installing:

```sh
zypper install kernel-default
```

### openSUSE Slowroll

Slowroll's own `update-slowroll` repository carries openSUSE's packaged
mhVTL (1.72) at a raised priority (80), and zypper prefers it over the newer
packages here. Give this repository a higher priority first:

```sh
zypper addrepo --refresh \
    https://download.opensuse.org/repositories/home:/ciriarte:/mhvtl/openSUSE_Slowroll/ mhvtl
zypper modifyrepo --priority 70 mhvtl
zypper --gpg-auto-import-keys refresh mhvtl
zypper install mhvtl-utils mhvtl-kmp-default
```

(`zypper install --from mhvtl mhvtl-utils mhvtl-kmp-default` works too, for a
one-off install.)

### Rocky Linux, AlmaLinux, RHEL 9 and 10

DKMS comes from EPEL. The module is built against the running kernel, so its
`kernel-devel` must be installed.

```sh
dnf install epel-release
curl -o /etc/yum.repos.d/mhvtl.repo \
    https://download.opensuse.org/repositories/home:/ciriarte:/mhvtl/RockyLinux_9/home:ciriarte:mhvtl.repo
dnf install kernel-devel-$(uname -r) mhvtl-utils mhvtl-dkms
```

Use `RockyLinux_10`, `AlmaLinux_9` or `AlmaLinux_10` as appropriate. If
`epel-release` is not in the distribution's repositories (RHEL), install it
from <https://dl.fedoraproject.org/pub/epel/epel-release-latest-9.noarch.rpm>
(or `-10`).

If `kernel-devel-$(uname -r)` is not found, the running kernel is older than
the one in the repositories: update, reboot, and try again.

### Debian 13, Ubuntu 24.04 and 26.04

```sh
install -d /etc/apt/keyrings
curl -o /etc/apt/keyrings/mhvtl.asc \
    https://download.opensuse.org/repositories/home:/ciriarte:/mhvtl/Debian_13/Release.key
echo "deb [signed-by=/etc/apt/keyrings/mhvtl.asc] https://download.opensuse.org/repositories/home:/ciriarte:/mhvtl/Debian_13/ /" \
    > /etc/apt/sources.list.d/mhvtl.list
apt-get update
apt-get install linux-headers-$(uname -r) mhvtl
```

Use `xUbuntu_24.04` or `xUbuntu_26.04` in both URLs on Ubuntu. Installing
`mhvtl` pulls in `mhvtl-dkms`, which builds the module.

**Ubuntu cloud and virtual kernels:** the SCSI tape driver `st` is not in the
base module set there; it ships in `linux-modules-extra`. Without it the
drives have no `/dev/st*` or `/dev/nst*` nodes (the changer and `/dev/sg*` still
work). Check, and install it if needed:

```sh
modinfo st >/dev/null 2>&1 || apt-get install linux-modules-extra-$(uname -r)
```

### Proxmox VE 9 and 8

Proxmox VE runs its own kernel, and there is no `linux-headers-$(uname -r)` package for
it. Install Proxmox's header metapackage **before** mhVTL, so DKMS finds the
headers of the running kernel:

```sh
apt-get install proxmox-default-headers
```

Then follow the Debian steps above without `linux-headers-$(uname -r)`: use
`Debian_13` for Proxmox VE 9 and `Debian_12` for Proxmox VE 8 (see the note
under *Supported releases*).

Installing mhVTL first does not work on Proxmox VE 8. There, Debian's `dkms` package
recommends `linux-headers-amd64 | linux-headers-generic | linux-headers`, and
apt satisfies that with an old `pve-headers` package that does not match the
running kernel. DKMS then has no headers to build against.

Without a subscription, the `pve-enterprise` (and, on Proxmox VE 9, `ceph`
enterprise) repositories return 401 and `apt-get update` fails. Disable them
or switch to the `pve-no-subscription` repository first:
<https://pve.proxmox.com/wiki/Package_Repositories>.

## Start and check

The packages do not start mhVTL on their own: it needs a configured library
first. The default configuration (`/etc/mhvtl/device.conf`) defines two
libraries with four drives each.

```sh
systemctl start mhvtl.target
lsscsi -g
```

`lsscsi` lists one `mediumx` (changer) per library and one `tape` per drive.
The cartridges listed in `/etc/mhvtl/library_contents.*` are created under
`/var/lib/mhvtl` when the libraries start.

To exercise the changer and a drive (`mtx` and `mt` come from the
distribution's `mtx` and `mt-st` packages):

```sh
mtx -f /dev/sgN status      # sgN = the mediumx line of lsscsi -g
mtx -f /dev/sgN load 1 0    # slot 1 into drive 0
mt -f /dev/nstM status      # nstM = that drive's tape node
```

On openSUSE, `mt-st` is no longer packaged; Leap 16.0 also lacks `mtx`,
which the `hardware` project builds:
<https://download.opensuse.org/repositories/hardware/16.0/>.

To start mhVTL at boot:

```sh
systemctl enable mhvtl.target
```

## Secure Boot

With Secure Boot enabled the kernel only loads signed modules. For DKMS,
follow the "Secure Boot" section of `INSTALL`: DKMS signs the module with its
own key, which has to be enrolled once as a machine owner key (MOK).

The openSUSE KMPs and the Secure Boot path have **not been tested** with these
packages; all validation so far ran with Secure Boot disabled.

## Updates

The packages are snapshots of upstream `master`, versioned
`1.8.0+git<date>.<commit>`. A new snapshot should arrive through the normal
`zypper update`, `dnf upgrade` or `apt upgrade`, with DKMS rebuilding the
module and openSUSE installing the matching KMP, and configuration in
`/etc/mhvtl` kept. Upgrading between snapshots has not been tested yet.

## Remove

```sh
systemctl disable --now mhvtl.target
zypper remove mhvtl-utils mhvtl-kmp-default   # openSUSE
dnf remove mhvtl-utils mhvtl-dkms             # Rocky, AlmaLinux, RHEL
apt-get purge mhvtl mhvtl-dkms                # Debian, Ubuntu
```

Removal stops the daemons and removes the kernel module and units. Cartridge
data under `/var/lib/mhvtl` is kept; delete it by hand if no longer needed.

## Validation record

Snapshot `1.8.0+git20261015.f3787bf`, 2026-10-15, one fresh VM per release.
Every step above was run as written, then checked: module present for the
running kernel and loaded; `mhvtl.target` active with all 10 devices; no failed
units; 72 cartridges created; `mtx status`; upstream
`tests/integration_tape_backup_restore.sh` (tar across two drives, md5
compare); a 1 KiB block read back through a 64 KiB buffer; the library back
with its media after `systemctl enable` and a reboot; clean removal.

| Release | Kernel | Result |
|---|---|---|
| openSUSE Tumbleweed (20261014) | 7.2.10-1-default | pass |
| openSUSE Slowroll (20261001) | 7.2.9-1.0.16.sr20260901-default | pass (with the repository priority above) |
| openSUSE Leap 16.0 | 6.12.0-160000.38-default | pass |
| Rocky Linux 9.8 | 5.14.0-687.57.1.el9_8 | pass |
| AlmaLinux 9.8 | 5.14.0-687.57.1.el9_8 | pass |
| Rocky Linux 10.2 | 6.12.0-211.65.1.el10_2 | pass |
| AlmaLinux 10.2 | 6.12.0-211.65.1.el10_2 | pass |
| Debian 13 | 6.12.111+deb13-amd64 | pass |
| Ubuntu 24.04.5 | 6.8.0-147-generic | pass (with `linux-modules-extra`) |
| Ubuntu 26.04.1 | 7.0.0-38-generic | pass |
| Proxmox VE 9.2.21 | 7.0.14-24-pve | pass (`proxmox-default-headers` first) |

Built from the `packaging/debian12-units` branch (same sources plus the unit
directory fix), 2026-10-15/16, because `Debian_12` does not publish yet:

| Release | Kernel | Result |
|---|---|---|
| Debian 12 | 6.1.0-53-amd64 | pass |
| Proxmox VE 8.4.21 | 6.8.12-43-pve | pass (`proxmox-default-headers` first) |

Not covered: RHEL itself (the Rocky and Alma builds are expected to work
there; OBS has no RHEL 9/10 build target), Secure Boot, aarch64, upgrades
from openSUSE's own packaged mhVTL 1.72, upgrades between snapshots.
