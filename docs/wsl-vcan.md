# SocketCAN on WSL2: enabling `vcan` (optional)

Nothing in this project needs this step. The node and every test run on the UDP backend,
which needs no kernel support, and CI exercises real SocketCAN on GitHub's runners. This page
is for running the SocketCAN backend on a Windows laptop as well.

## The check

On the stock WSL2 kernel:

```bash
sudo modprobe vcan; echo "exit status $?"
```

fails with status 1. The reason is in the kernel's configuration:

```bash
zcat /proc/config.gz | grep -E 'CONFIG_CAN(_RAW|_DEV|_VCAN)?[= ]'
```

On kernel `6.6.87.2-microsoft-standard-WSL2` this prints `CONFIG_CAN=m`, `CONFIG_CAN_RAW=m`,
`CONFIG_CAN_DEV=m` and `# CONFIG_CAN_VCAN is not set`: Microsoft ships the CAN core and the
raw protocol as loadable modules but does not build the virtual interface driver.

## The lightest fix: build only that one module

Replacing the WSL kernel with a custom build works, but it is a long build, a machine-wide
change, and a kernel that no longer follows Microsoft's updates. The module alone is enough:
its source is in Microsoft's public kernel tree, and a module built from the exact source tag
with the running kernel's own configuration loads into the stock kernel.

```bash
scripts/wsl/build-vcan-module.sh
```

The script installs the kernel build prerequisites with apt, downloads the source of the
running kernel release from `github.com/microsoft/WSL2-Linux-Kernel`, enables
`CONFIG_CAN_VCAN=m` on top of `/proc/config.gz`, and builds `drivers/net/can` only. It needs
about 250 MB of download and 1.5 GB of disk for the kernel source tree, took about a minute on
the 8-core development laptop, and leaves `~/.local/share/fcn/vcan-<kernel release>.ko`.

Then, and again whenever the WSL virtual machine has restarted:

```bash
scripts/vcan-up.sh --verify
```

loads the CAN core modules and the built module, creates `vcan0` and proves it with a frame
sent by `cansend` and received by `candump`.

## What to expect

- **Not persistent.** A loaded module lives in the WSL virtual machine's kernel and is gone
  when that machine restarts: after `wsl --shutdown`, after a Windows restart, and also about
  a minute after the last WSL process has exited, because WSL stops an idle machine. Run
  `scripts/vcan-up.sh` at the start of a session; it does nothing if `vcan0` is already up.
- **Tied to the kernel release.** After a WSL kernel update the module has to be built again;
  the file name carries the release, so an old module is never loaded into a new kernel.
- **One warning in the kernel log.** `vcan: no symbol version for module_layout`, followed
  by a note that an out-of-tree module taints the kernel. The stock kernel is built with
  symbol versioning, but Microsoft does not publish the version table a module build would
  need, so the module carries none for kernel symbols and the loader accepts it with that
  warning. The comment at the top of the build script explains why that is sound here.
- **Shared by all WSL distributions.** WSL2 runs every distribution on one kernel, so the
  module is visible from all of them, Docker Desktop's included.

## Without it

The UDP backend is the default and needs no kernel support. System tests that need SocketCAN
are marked `socketcan` and are skipped, not failed, when `vcan0` does not exist.
