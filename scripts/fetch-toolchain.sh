#!/usr/bin/env bash
# Download the pinned portable GCC used for dimension 3 and lay it out so
# <dest>/bin/g++ exists (xPack nests a versioned top-level directory).
#
# Pin checked 2026-10-31 against the upstream release asset. The plan's
# example URL v14.2.0-3 returns HTTP 404. v14.3.0-1 is the current GCC 14
# linux-x64 build. SHA-256 is the contents of the upstream .sha file, not
# a hand-written digest:
#   https://github.com/xpack-dev-tools/gcc-xpack/releases/download/v14.3.0-1/xpack-gcc-14.3.0-1-linux-x64.tar.gz.sha
set -euo pipefail

TOOLCHAIN_URL="https://github.com/xpack-dev-tools/gcc-xpack/releases/download/v14.3.0-1/xpack-gcc-14.3.0-1-linux-x64.tar.gz"
TOOLCHAIN_SHA256="93bc0add266a50626861d571cce92667b8d42e2770f3f321b3c8df096cee5bf1"

if [[ $# -ne 1 || -z "${1}" ]]; then
  echo "usage: $0 <dest_dir>" >&2
  exit 2
fi

dest=$(realpath -m "$1")
case "${dest}" in
  /|/usr|/usr/*|/bin|/lib|/lib64|/opt)
    echo "refusing to use dest ${dest}" >&2
    exit 2
    ;;
esac

for cmd in curl sha256sum tar find; do
  if ! command -v "${cmd}" >/dev/null 2>&1; then
    echo "missing required command: ${cmd}" >&2
    exit 1
  fi
done

tmp=$(mktemp -d)
cleanup() {
  rm -rf "${tmp}"
}
trap cleanup EXIT

archive="${tmp}/gcc.tar.gz"
echo "downloading ${TOOLCHAIN_URL}"
curl --fail --location --retry 5 --retry-delay 2 --output "${archive}" "${TOOLCHAIN_URL}"

echo "${TOOLCHAIN_SHA256}  ${archive}" | sha256sum -c -

mkdir -p "${tmp}/extract"
tar -xzf "${archive}" -C "${tmp}/extract"

best=""
best_len=999999
while IFS= read -r -d '' gxx; do
  if [[ ! -x "${gxx}" ]]; then
    continue
  fi
  parent=$(dirname "$(dirname "${gxx}")")
  len=${#parent}
  if (( len < best_len )); then
    best="${parent}"
    best_len=${len}
  fi
done < <(find "${tmp}/extract" \( -type f -o -type l \) -path '*/bin/g++' -print0)

if [[ -z "${best}" ]]; then
  echo "archive has no executable bin/g++" >&2
  find "${tmp}/extract" -maxdepth 3 -type d -print >&2 || true
  exit 1
fi

rm -rf "${dest}"
mkdir -p "${dest}"
cp -a "${best}/." "${dest}/"

if [[ ! -x "${dest}/bin/g++" ]]; then
  echo "install failed: ${dest}/bin/g++ is not executable" >&2
  exit 1
fi

echo "toolchain ready: ${dest}"
"${dest}/bin/g++" --version > "${tmp}/gxx-version.txt"
head -n 1 "${tmp}/gxx-version.txt"
