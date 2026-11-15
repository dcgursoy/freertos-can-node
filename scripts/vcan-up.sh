#!/usr/bin/env bash
# Bring up a virtual SocketCAN interface and, with --verify, prove that it carries a frame.
#   scripts/vcan-up.sh [--verify] [--install-modules-extra] [interface]     (default: vcan0)
#
# Needs root for the kernel module and the interface; uses sudo unless it already is root.
#
# Where the vcan module comes from, in this order:
#   1. the running kernel (modprobe vcan): any normal Linux, and GitHub's runners
#   2. with --install-modules-extra: the distribution's linux-modules-extra package for the
#      running kernel, then modprobe again (older runner images kept vcan there)
#   3. a module file built for the running kernel, named by FCN_VCAN_KO or found at
#      ~/.local/share/fcn/vcan-<kernel release>.ko: the way to get vcan on the stock WSL2
#      kernel, which is built without it (docs/wsl-vcan.md)
set -euo pipefail

verify=0
install_extra=0
iface="vcan0"
for arg in "$@"; do
    case "$arg" in
        --verify) verify=1 ;;
        --install-modules-extra) install_extra=1 ;;
        -*) echo "unknown option: $arg" >&2; exit 2 ;;
        *) iface="$arg" ;;
    esac
done

sudo_cmd=()
[[ "$(id -u)" -ne 0 ]] && sudo_cmd=(sudo -n)

load_vcan() {
    "${sudo_cmd[@]}" modprobe vcan 2>/dev/null && return 0

    if [[ "$install_extra" -eq 1 ]]; then
        echo "vcan is not in the running kernel's modules; installing linux-modules-extra"
        "${sudo_cmd[@]}" apt-get update -qq
        "${sudo_cmd[@]}" apt-get install -y -qq "linux-modules-extra-$(uname -r)" &&
            "${sudo_cmd[@]}" modprobe vcan && return 0
    fi

    local ko="${FCN_VCAN_KO:-$HOME/.local/share/fcn/vcan-$(uname -r).ko}"
    if [[ -f "$ko" ]]; then
        echo "loading $ko"
        "${sudo_cmd[@]}" modprobe can
        "${sudo_cmd[@]}" modprobe can_raw
        "${sudo_cmd[@]}" modprobe can_dev
        "${sudo_cmd[@]}" insmod "$ko" && return 0
    fi

    return 1
}

if ! ip link show "$iface" >/dev/null 2>&1; then
    if ! grep -q '^vcan ' /proc/modules 2>/dev/null; then
        if ! load_vcan; then
            echo "error: no vcan module for kernel $(uname -r)." >&2
            echo "       On WSL2 see docs/wsl-vcan.md; the UDP backend needs no kernel support." >&2
            exit 1
        fi
    fi
    "${sudo_cmd[@]}" ip link add dev "$iface" type vcan
fi
"${sudo_cmd[@]}" ip link set up "$iface"
ip -brief link show "$iface"

if [[ "$verify" -eq 1 ]]; then
    # One frame sent with cansend has to arrive in candump.
    log="$(mktemp)"
    timeout 5 candump -n 1 "$iface,123:7FF" >"$log" 2>&1 &
    dump_pid=$!
    sleep 0.5
    cansend "$iface" 123#DEADBEEF
    if wait "$dump_pid" && grep -q ' 123 ' "$log"; then
        echo "verified: $(tr -s ' ' <"$log")"
        rm -f "$log"
    else
        echo "error: the frame sent on $iface did not come back" >&2
        cat "$log" >&2
        rm -f "$log"
        exit 1
    fi
fi
