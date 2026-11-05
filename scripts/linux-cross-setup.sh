#!/usr/bin/env bash
# Prepare an Ubuntu x64 host (the CI runner, or your own machine) to cross-compile gn-cli
# for another Linux architecture with Ubuntu's multiarch packages, and to run the result
# under QEMU user-mode emulation.
#
#   sudo scripts/linux-cross-setup.sh armhf      # or riscv64, ppc64el, s390x, arm64
#   cmake -S . -B build $(scripts/linux-cross-setup.sh --cmake-args armhf)
#
# Ubuntu publishes amd64/i386 on archive.ubuntu.com (and the Azure mirror the runners use)
# and every other architecture on ports.ubuntu.com, so the existing sources are pinned to
# amd64/i386 and a ports source is added for the target architecture.
set -euo pipefail

triplet_of() {
  case "$1" in
    armhf)   echo arm-linux-gnueabihf ;;
    arm64)   echo aarch64-linux-gnu ;;
    riscv64) echo riscv64-linux-gnu ;;
    ppc64el) echo powerpc64le-linux-gnu ;;
    s390x)   echo s390x-linux-gnu ;;
    *) echo "unsupported architecture: $1 (armhf, arm64, riscv64, ppc64el, s390x)" >&2; exit 2 ;;
  esac
}

processor_of() {
  case "$1" in
    armhf) echo armv7l ;; arm64) echo aarch64 ;; ppc64el) echo ppc64le ;; *) echo "$1" ;;
  esac
}

if [ "${1:-}" = --cmake-args ]; then
  arch="${2:?usage: $0 --cmake-args <debian arch>}"
  triplet="$(triplet_of "$arch")"
  echo "-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=$(processor_of "$arch")" \
       "-DCMAKE_CXX_COMPILER=$triplet-g++ -DCMAKE_LIBRARY_ARCHITECTURE=$triplet"
  exit 0
fi

arch="${1:?usage: $0 <debian arch>}"
triplet="$(triplet_of "$arch")"
. /etc/os-release
codename="${UBUNTU_CODENAME:-$VERSION_CODENAME}"

# Pin what's there to the host architectures; deb822 sources first, then one-line ones.
shopt -s nullglob
for f in /etc/apt/sources.list.d/*.sources; do
  [ "$(basename "$f")" = cross-ports.sources ] && continue
  grep -qi '^Architectures:' "$f" || sed -i '/^URIs:/a Architectures: amd64 i386' "$f"
done
for f in /etc/apt/sources.list /etc/apt/sources.list.d/*.list; do
  sed -i -E 's/^deb(-src)? (http|https|mirror)/deb\1 [arch=amd64,i386] \2/' "$f"
  sed -i -E '/^deb(-src)? \[/{/arch=/!s/^deb(-src)? \[/deb\1 [arch=amd64,i386 /}' "$f"
done

cat > /etc/apt/sources.list.d/cross-ports.sources <<EOF
Types: deb
URIs: http://ports.ubuntu.com/ubuntu-ports
Suites: $codename $codename-updates $codename-security
Components: main universe
Architectures: $arch
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF

# A mirror that stalls mid-download (seen on azure.archive.ubuntu.com) would otherwise hold apt
# forever: give up on a silent connection after 30 s, retry each file, and bound every apt run.
cat > /etc/apt/apt.conf.d/80-gn-cli-retries <<'EOF'
Acquire::Retries "5";
Acquire::http::Timeout "30";
Acquire::https::Timeout "30";
EOF
export DEBIAN_FRONTEND=noninteractive
apt_retry() {
  local attempt
  for attempt in 1 2 3; do
    timeout 300 "$@" && return 0
    echo "apt attempt $attempt failed or timed out; retrying" >&2
    dpkg --configure -a || true  # in case the timeout cut an unpack short
    sleep $((attempt * 10))
  done
  return 1
}

dpkg --add-architecture "$arch"
apt_retry apt-get update -q
# libcurl4-openssl-dev isn't co-installable across architectures (each ships its own
# /usr/bin/curl-config), and the cross build doesn't want the host's anyway.
host="$(dpkg --print-architecture)"
if [ "$host" != "$arch" ] && dpkg -s "libcurl4-openssl-dev:$host" >/dev/null 2>&1; then
  DEBIAN_FRONTEND=noninteractive apt-get remove -y -q "libcurl4-openssl-dev:$host"
fi
apt_retry apt-get install -y -q --no-install-recommends \
  "g++-$triplet" "libcurl4-openssl-dev:$arch" "libstdc++6:$arch" nlohmann-json3-dev qemu-user-static binfmt-support

# Multiarch puts the target's dynamic loader and libraries where the binary expects them,
# so with a binfmt handler for QEMU it runs like a native one. Ubuntu ships the handlers
# as systemd-binfmt files; register them directly where systemd didn't (containers).
binfmt=/proc/sys/fs/binfmt_misc
[ -e "$binfmt/register" ] || mount -t binfmt_misc binfmt_misc "$binfmt" 2>/dev/null || true
for conf in /usr/lib/binfmt.d/qemu-*.conf; do
  name="$(cut -d: -f2 "$conf")"
  [ -e "$binfmt/$name" ] || cat "$conf" > "$binfmt/register" 2>/dev/null \
    || echo "warning: could not register $name; run the binary with qemu-<arch>-static" >&2
done
echo "Ready to cross-compile for $arch ($triplet)"
