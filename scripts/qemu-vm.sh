#!/usr/bin/env bash
# QNX 8.0 x86_64 test VM for rtcollector, on a Linux host with QNX SDP 8.0.
#
#   scripts/qemu-vm.sh build     build the image with mkqnximage (in $VM_DIR)
#   scripts/qemu-vm.sh run       start QEMU in the background
#   scripts/qemu-vm.sh stop      stop it
#   scripts/qemu-vm.sh status
#
# mkqnximage only builds: its own runner needs a host bridge (brctl, setuid
# qemu-bridge-helper, br0, dnsmasq). This runs QEMU with user-mode
# networking instead. From the guest, the host is 10.0.2.2 (Arc at
# 10.0.2.2:8000). The serial console is a TCP port on the host; drive it
# with scripts/qnx-serial.py. ssh on the default image is broken (sshd-auth
# is missing), so files are pulled from the host over HTTP with the target's
# own curl.
set -euo pipefail

VM_DIR=${VM_DIR:-$HOME/qnxvm}
SDP=${QNX_SDP:-$HOME/qnx800}
CPUS=${VM_CPUS:-4}
RAM=${VM_RAM:-1G}
SERIAL_PORT=${VM_SERIAL_PORT:-5555}
SSH_PORT=${VM_SSH_PORT:-2222}

sdp() { # shellcheck disable=SC1091
	source "$SDP/qnxsdp-env.sh" >/dev/null
}

case "${1:-}" in
build)
	sdp
	mkdir -p "$VM_DIR"
	cd "$VM_DIR"
	mkqnximage --noprompt --type=qemu --arch=x86_64 --cpu="$CPUS" --ram="$RAM" \
		--hostname=qnxvm --users=+rtuser/rtuser --ip=dhcp --build
	;;
run)
	cd "$VM_DIR"
	if [ -f output/qemu.pid ] && kill -0 "$(cat output/qemu.pid)" 2>/dev/null; then
		echo "already running (pid $(cat output/qemu.pid))"
		exit 0
	fi
	accel=(-cpu max)
	if [ -w /dev/kvm ]; then accel=(-enable-kvm -cpu host); else echo "no write access to /dev/kvm: using TCG (slow)"; fi
	qemu-system-x86_64 -smp "$CPUS" "${accel[@]}" -m "$RAM" \
		-kernel output/ifs.bin \
		-drive file=output/disk-qemu.vmdk,if=ide,id=drv0 \
		-netdev "user,id=net0,hostfwd=tcp:127.0.0.1:${SSH_PORT}-:22" \
		-device virtio-net-pci,netdev=net0 \
		-object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 \
		-display none -monitor none \
		-serial "tcp:127.0.0.1:${SERIAL_PORT},server,nowait" \
		-pidfile output/qemu.pid -daemonize
	echo "started; serial console on 127.0.0.1:${SERIAL_PORT}"
	;;
stop)
	cd "$VM_DIR"
	if [ -f output/qemu.pid ]; then kill "$(cat output/qemu.pid)" 2>/dev/null || true; rm -f output/qemu.pid; fi
	;;
status)
	cd "$VM_DIR"
	if [ -f output/qemu.pid ] && kill -0 "$(cat output/qemu.pid)" 2>/dev/null; then echo "running (pid $(cat output/qemu.pid))"; else echo stopped; fi
	;;
*)
	sed -n '2,15p' "$0"
	exit 2
	;;
esac
