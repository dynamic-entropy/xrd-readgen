#!/usr/bin/env bash
# Install xrd-readgen from GitHub Releases into /usr/local (override with PREFIX).
#
#   curl -fsSL https://github.com/dynamic-entropy/xrd-readgen/releases/latest/download/install.sh | sudo bash
#   VERSION=0.1.0 ./install.sh
#
set -euo pipefail

REPO="${REPO:-dynamic-entropy/xrd-readgen}"
PREFIX="${PREFIX:-/usr/local}"
CONFIG_DIR="${CONFIG_DIR:-/etc/xrd-readgen}"
RESULTS_DIR="${RESULTS_DIR:-/var/lib/xrd-readgen/results}"
ARCH="${ARCH:-linux-amd64}"
VERSION="${VERSION:-}"
TMPDIR="${TMPDIR:-/tmp}"

MISSING=()
HINTS=()

add_missing() {
  MISSING+=("$1")
  shift
  if [[ $# -gt 0 ]]; then
    HINTS+=("$*")
  fi
}

have_cmd() { command -v "$1" >/dev/null 2>&1; }

have_lib() {
  # $1 = soname stem, e.g. libXrdCl.so
  local name="$1"
  if have_cmd ldconfig && ldconfig -p 2>/dev/null | grep -F "${name}" >/dev/null; then
    return 0
  fi
  local p
  for p in "/usr/lib64/${name}" "/usr/lib/${name}"; do
    if [[ -e "$p" ]]; then
      return 0
    fi
  done
  # Also accept versioned sonames: libXrdCl.so.3
  if compgen -G "/usr/lib64/${name}.*" >/dev/null 2>&1; then
    return 0
  fi
  if compgen -G "/usr/lib/${name}.*" >/dev/null 2>&1; then
    return 0
  fi
  return 1
}

pkg_hint() {
  # Best-effort package manager hint for el/fedora-like hosts.
  if have_cmd dnf; then
    echo "sudo dnf install -y $*"
  elif have_cmd yum; then
    echo "sudo yum install -y $*"
  elif have_cmd apt-get; then
    echo "sudo apt-get install -y $*"
  else
    echo "install packages: $*"
  fi
}

echo "==> checking prerequisites"

if [[ "$(uname -s)" != "Linux" ]]; then
  add_missing "OS is $(uname -s), need Linux" \
    "run this installer on linux/amd64 (e.g. cmsxrd-readgen-amd64 / el10)"
fi

host_arch="$(uname -m)"
if [[ "$host_arch" != "x86_64" && "$host_arch" != "amd64" ]]; then
  add_missing "CPU arch is ${host_arch}, need x86_64" \
    "this release binary is ${ARCH} only"
fi

have_cmd curl || add_missing "command 'curl'" "$(pkg_hint curl)"
have_cmd tar || add_missing "command 'tar'" "$(pkg_hint tar)"
have_cmd install || add_missing "command 'install' (coreutils)" "$(pkg_hint coreutils)"

# Optional but recommended for checksum verification.
if ! have_cmd sha256sum && ! have_cmd shasum; then
  echo "warning: neither sha256sum nor shasum found; download integrity check will be skipped" >&2
fi

have_lib 'libXrdCl.so' || add_missing "library libXrdCl (XRootD client)" \
  "$(pkg_hint epel-release); $(pkg_hint xrootd-client)  # el10: also 'dnf config-manager --set-enabled crb' if needed"
have_lib 'libcurl.so' || add_missing "library libcurl" "$(pkg_hint libcurl)"
have_lib 'libcrypto.so' || add_missing "library libcrypto (OpenSSL)" "$(pkg_hint openssl-libs)"

# Need write access to install destinations (usually root).
for d in "$PREFIX" "$CONFIG_DIR" "$RESULTS_DIR"; do
  parent="$d"
  while [[ ! -e "$parent" && "$parent" != "/" ]]; do
    parent="$(dirname "$parent")"
  done
  if [[ ! -w "$parent" ]]; then
    add_missing "write access to ${d} (via ${parent})" \
      "re-run as root: curl -fsSL .../install.sh | sudo bash"
    break
  fi
done

if [[ ${#MISSING[@]} -gt 0 ]]; then
  echo "error: missing prerequisites:" >&2
  i=0
  for m in "${MISSING[@]}"; do
    echo "  - ${m}" >&2
    if [[ $i -lt ${#HINTS[@]} && -n "${HINTS[$i]}" ]]; then
      echo "      fix: ${HINTS[$i]}" >&2
    fi
    i=$((i + 1))
  done
  echo >&2
  echo "On AlmaLinux/RHEL 10 (el10), a typical bootstrap is:" >&2
  echo "  sudo dnf install -y epel-release curl tar coreutils" >&2
  echo "  sudo dnf config-manager --set-enabled crb   # if needed" >&2
  echo "  sudo dnf install -y xrootd-client libcurl openssl-libs" >&2
  echo "  curl -fsSL https://github.com/${REPO}/releases/latest/download/install.sh | sudo bash" >&2
  exit 1
fi

echo "  ok: Linux/${host_arch}, curl, tar, libXrdCl, libcurl, libcrypto"

WORKDIR="$(mktemp -d "${TMPDIR%/}/xrd-readgen-install.XXXXXX")"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

api="https://api.github.com/repos/${REPO}/releases"
if [[ -n "$VERSION" ]]; then
  tag="v${VERSION#v}"
  if ! release_json="$(curl -fsSL "${api}/tags/${tag}")"; then
    echo "error: could not fetch release ${tag} from GitHub (${REPO})" >&2
    echo "  fix: check network / tag exists: https://github.com/${REPO}/releases" >&2
    exit 1
  fi
else
  if ! release_json="$(curl -fsSL "${api}/latest")"; then
    echo "error: could not fetch latest release from GitHub (${REPO})" >&2
    echo "  fix: check network: https://github.com/${REPO}/releases" >&2
    exit 1
  fi
  tag="$(printf '%s' "$release_json" | sed -n 's/.*"tag_name":[[:space:]]*"\([^"]*\)".*/\1/p' | head -1)"
  VERSION="${tag#v}"
fi

if [[ -z "$VERSION" ]]; then
  echo "error: could not resolve release version for ${REPO}" >&2
  echo "  fix: set VERSION=0.1.0 or publish a GitHub Release" >&2
  exit 1
fi

asset="xrd-readgen-${VERSION}-${ARCH}.tar.gz"
base="https://github.com/${REPO}/releases/download/v${VERSION}"
echo "==> downloading ${asset}"
if ! curl -fsSL -o "${WORKDIR}/${asset}" "${base}/${asset}"; then
  echo "error: failed to download ${base}/${asset}" >&2
  echo "  fix: confirm the asset exists on the release page" >&2
  exit 1
fi
if curl -fsSL -o "${WORKDIR}/SHA256SUMS" "${base}/SHA256SUMS"; then
  if have_cmd sha256sum; then
    (cd "$WORKDIR" && sha256sum -c SHA256SUMS --ignore-missing)
  elif have_cmd shasum; then
    expect="$(awk -v f="$asset" '$2 == f { print $1 }' "${WORKDIR}/SHA256SUMS")"
    actual="$(shasum -a 256 "${WORKDIR}/${asset}" | awk '{ print $1 }')"
    if [[ -n "$expect" && "$expect" != "$actual" ]]; then
      echo "error: SHA256 mismatch for ${asset}" >&2
      exit 1
    fi
  fi
else
  echo "warning: SHA256SUMS not available for v${VERSION}; skipping checksum" >&2
fi

echo "==> extracting"
tar -C "$WORKDIR" -xzf "${WORKDIR}/${asset}"
src="${WORKDIR}/xrd-readgen-${VERSION}-${ARCH}"
if [[ ! -x "${src}/bin/xrd-readgen" ]]; then
  echo "error: tarball missing bin/xrd-readgen" >&2
  exit 1
fi

# Catch unresolved dynamic deps before installing.
if have_cmd ldd; then
  unresolved="$(ldd "${src}/bin/xrd-readgen" 2>/dev/null | awk '/not found/ { print $1 }' || true)"
  if [[ -n "$unresolved" ]]; then
    echo "error: binary is missing shared libraries:" >&2
    printf '  - %s\n' $unresolved >&2
    echo "  fix: install matching runtime packages (usually xrootd-client, libcurl, openssl-libs)" >&2
    exit 1
  fi
fi

echo "==> installing to ${PREFIX}"
install -d "${PREFIX}/bin" "${PREFIX}/share"
install -m 0755 "${src}/bin/xrd-readgen" "${PREFIX}/bin/xrd-readgen"
if [[ -f "${src}/bin/multi_run.py" ]]; then
  install -m 0755 "${src}/bin/multi_run.py" "${PREFIX}/bin/multi_run.py"
fi
if [[ -f "${src}/bin/capacity_sweep.py" ]]; then
  install -m 0755 "${src}/bin/capacity_sweep.py" "${PREFIX}/bin/capacity_sweep.py"
fi
rm -rf "${PREFIX}/share/xrd-readgen"
cp -a "${src}/share/xrd-readgen" "${PREFIX}/share/"

echo "==> ensuring config/results dirs"
install -d -m 0755 \
  "${CONFIG_DIR}/workloads" \
  "${CONFIG_DIR}/filelists" \
  "${RESULTS_DIR}"

# Seed sanitized examples into /etc only when absent (never overwrite operator files).
seed_file() {
  local from="$1" to="$2"
  if [[ ! -e "$to" ]]; then
    install -D -m 0644 "$from" "$to"
    echo "  seeded ${to}"
  fi
}
share="${PREFIX}/share/xrd-readgen"
seed_file "${share}/workloads/example.json" "${CONFIG_DIR}/workloads/example.json"
seed_file "${share}/workloads/example_workload.json" "${CONFIG_DIR}/workloads/example_workload.json"
seed_file "${share}/workloads/example_uncapped.json" "${CONFIG_DIR}/workloads/example_uncapped.json"
seed_file "${share}/filelists/local.txt" "${CONFIG_DIR}/filelists/local.txt"
seed_file "${share}/filelists/remote.txt" "${CONFIG_DIR}/filelists/remote.txt"
seed_file "${share}/filelists/remote_single.txt" "${CONFIG_DIR}/filelists/remote_single.txt"

echo "==> done"
echo "  binary:  ${PREFIX}/bin/xrd-readgen"
if [[ -x "${PREFIX}/bin/multi_run.py" ]]; then
  echo "  multi_run.py:       ${PREFIX}/bin/multi_run.py"
fi
if [[ -x "${PREFIX}/bin/capacity_sweep.py" ]]; then
  echo "  capacity_sweep.py:  ${PREFIX}/bin/capacity_sweep.py"
fi
echo "  config:  ${CONFIG_DIR}"
echo "  results: ${RESULTS_DIR}"
if ! reported="$("${PREFIX}/bin/xrd-readgen" version)"; then
  echo "error: installed binary failed to run 'version'" >&2
  echo "  fix: check 'ldd ${PREFIX}/bin/xrd-readgen' for missing libraries" >&2
  exit 1
fi
echo "  ${reported}"
bin_ver="$(awk '{print $2}' <<<"$reported")"
if [[ -n "$bin_ver" && "$bin_ver" != "$VERSION" ]]; then
  echo "error: installed binary reports ${bin_ver} but release asset is ${VERSION}" >&2
  echo "  fix: rebuild the release with VERSION=${VERSION} passed to cmake" >&2
  exit 1
fi
