#!/usr/bin/env bash
# Assemble ps5ai-11dim-linux-amd64.tar.gz plus a sibling SHA256SUMS.
#
# Layout inside the archive:
#   ps5ai-11dim-linux-amd64/ps5ai-11dim
#   ps5ai-11dim-linux-amd64/toolchain/...     (bin/g++ beside the binary)
#   ps5ai-11dim-linux-amd64/README-run.txt
#   ps5ai-11dim-linux-amd64/SHA256SUMS        (hashes of the files above)
#
# <out_dir>/SHA256SUMS is the hash of the tar.gz itself (the Release asset).
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <build_dir> <toolchain_dir> <out_dir>" >&2
  exit 2
fi

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build=$(realpath "$1")
toolchain=$(realpath "$2")
out=$(realpath -m "$3")
name="ps5ai-11dim-linux-amd64"
binary="${build}/ps5ai-11dim"
readme="${root}/packaging/README-run.txt"

if [[ ! -f "${binary}" ]]; then
  echo "missing binary: ${binary}" >&2
  exit 1
fi
if [[ ! -x "${toolchain}/bin/g++" ]]; then
  echo "missing toolchain compiler: ${toolchain}/bin/g++" >&2
  exit 1
fi
if [[ ! -f "${readme}" ]]; then
  echo "missing ${readme}" >&2
  exit 1
fi

stage_parent=$(mktemp -d)
cleanup() {
  rm -rf "${stage_parent}"
}
trap cleanup EXIT

stage="${stage_parent}/${name}"
mkdir -p "${stage}/toolchain"
cp "${binary}" "${stage}/ps5ai-11dim"
chmod 0755 "${stage}/ps5ai-11dim"
if command -v strip >/dev/null 2>&1; then
  strip --strip-unneeded "${stage}/ps5ai-11dim" || echo "warning: strip failed; shipping unstripped binary" >&2
fi
cp -a "${toolchain}/." "${stage}/toolchain/"
cp "${readme}" "${stage}/README-run.txt"

(
  cd "${stage}"
  while IFS= read -r -d '' file; do
    sha256sum "${file}"
  done < <(find . -type f ! -name SHA256SUMS -print0 | sort -z) > SHA256SUMS
)

mkdir -p "${out}"
tar -C "${stage_parent}" -czf "${out}/${name}.tar.gz" "${name}"
(
  cd "${out}"
  sha256sum "${name}.tar.gz" > SHA256SUMS
)

list="${stage_parent}/listing.txt"
tar -tzf "${out}/${name}.tar.gz" > "${list}"
echo "packed ${out}/${name}.tar.gz"
head -n 20 "${list}"
echo "..."
missing=0
for needle in \
  "${name}/ps5ai-11dim" \
  "${name}/toolchain/bin/g++" \
  "${name}/SHA256SUMS" \
  "${name}/README-run.txt"
do
  if ! grep -Fxq "${needle}" "${list}"; then
    echo "archive missing ${needle}" >&2
    missing=1
  fi
done
if [[ "${missing}" -ne 0 ]]; then
  exit 1
fi
