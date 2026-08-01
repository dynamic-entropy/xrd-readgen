#!/usr/bin/env bash
# Build a linux/amd64 release tarball via Containerfile.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

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

echo "==> checking build prerequisites"

[[ -f Containerfile ]] || add_missing "Containerfile" "run from the repo root (missing ./Containerfile)"
[[ -d packaging/share/xrd-readgen ]] || add_missing "packaging/share/xrd-readgen" \
  "restore packaging/share examples used in the release tarball"
[[ -f scripts/install.sh ]] || add_missing "scripts/install.sh" "restore the install script"
[[ -f CMakeLists.txt ]] || add_missing "CMakeLists.txt" "run from the xrd-readgen repo root"

ENGINE=""
if have_cmd podman; then
  ENGINE=podman
elif have_cmd docker; then
  ENGINE=docker
else
  add_missing "container engine (podman or docker)" \
    "sudo dnf install -y podman   # or install Docker"
fi

have_cmd tar || add_missing "command 'tar'" "sudo dnf install -y tar"
if ! have_cmd sha256sum && ! have_cmd shasum; then
  add_missing "command 'sha256sum' (or shasum)" "sudo dnf install -y coreutils"
fi

if [[ ${#MISSING[@]} -gt 0 ]]; then
  echo "error: missing build prerequisites:" >&2
  i=0
  for m in "${MISSING[@]}"; do
    echo "  - ${m}" >&2
    if [[ $i -lt ${#HINTS[@]} && -n "${HINTS[$i]}" ]]; then
      echo "      fix: ${HINTS[$i]}" >&2
    fi
    i=$((i + 1))
  done
  exit 1
fi

VERSION="${VERSION:-}"
if [[ -z "$VERSION" ]]; then
  VERSION="$(sed -n 's/^[[:space:]]*VERSION[[:space:]]*\([0-9.]*\).*/\1/p' CMakeLists.txt | head -1)"
fi
if [[ -z "$VERSION" ]]; then
  echo "error: could not determine VERSION from CMakeLists.txt" >&2
  echo "  fix: set VERSION=0.1.0 explicitly" >&2
  exit 1
fi

ARCH="${ARCH:-linux-amd64}"
IMAGE_TAG="${IMAGE_TAG:-xrd-readgen-build:${VERSION}}"
DIST_DIR="${DIST_DIR:-${ROOT}/dist}"
STAGE="${DIST_DIR}/stage/xrd-readgen-${VERSION}-${ARCH}"
TARBALL="${DIST_DIR}/xrd-readgen-${VERSION}-${ARCH}.tar.gz"

echo "  ok: engine=${ENGINE}, version=${VERSION}, arch=${ARCH}"
if [[ "$(uname -m)" != "x86_64" && "$(uname -m)" != "amd64" ]]; then
  echo "note: host is $(uname -m); build uses --platform=linux/amd64 (needs qemu/binfmt)" >&2
fi

echo "==> building image ${IMAGE_TAG} with ${ENGINE} (platform linux/amd64)"
if ! "$ENGINE" build \
  --platform=linux/amd64 \
  -t "$IMAGE_TAG" \
  -f Containerfile \
  .; then
  echo "error: container build failed" >&2
  echo "  common fixes:" >&2
  echo "    - ensure qemu-user-static / binfmt is set up for amd64 on arm hosts" >&2
  echo "    - check network access to pull almalinux:10 and XRootD packages" >&2
  exit 1
fi

echo "==> extracting binary"
rm -rf "${DIST_DIR}/stage"
mkdir -p "${STAGE}/bin" "${STAGE}/share"
cid="$("$ENGINE" create --platform=linux/amd64 "$IMAGE_TAG")"
cleanup() { "$ENGINE" rm -f "$cid" >/dev/null 2>&1 || true; }
trap cleanup EXIT
"$ENGINE" cp "${cid}:/out/xrd-readgen" "${STAGE}/bin/xrd-readgen"
chmod 0755 "${STAGE}/bin/xrd-readgen"
install -m 0755 "${ROOT}/scripts/multi_run.py" "${STAGE}/bin/multi_run.py"
install -m 0755 "${ROOT}/scripts/capacity_sweep.py" "${STAGE}/bin/capacity_sweep.py"
cp -a packaging/share/xrd-readgen "${STAGE}/share/"

echo "==> packing ${TARBALL}"
mkdir -p "$DIST_DIR"
tar -C "${DIST_DIR}/stage" -czf "$TARBALL" "xrd-readgen-${VERSION}-${ARCH}"

(
  cd "$DIST_DIR"
  if have_cmd sha256sum; then
    sha256sum "xrd-readgen-${VERSION}-${ARCH}.tar.gz" > SHA256SUMS
  else
    shasum -a 256 "xrd-readgen-${VERSION}-${ARCH}.tar.gz" > SHA256SUMS
  fi
)

cp -f "${ROOT}/scripts/install.sh" "${DIST_DIR}/install.sh"
chmod 0755 "${DIST_DIR}/install.sh"

echo "Built:"
echo "  ${TARBALL}"
echo "  ${DIST_DIR}/SHA256SUMS"
echo "  ${DIST_DIR}/install.sh"
