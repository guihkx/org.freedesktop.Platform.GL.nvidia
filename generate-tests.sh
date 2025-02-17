#!/usr/bin/env bash

# Usage: ./generate-tests.sh version [arch]

set -euo pipefail

check_programs() {
  local not_installed=()
  for program in "${@}"; do
    if ! command -v "${program}" >/dev/null; then
      not_installed=("${program}")
    fi
  done
  if [ "${#not_installed[@]}" -gt 0 ]; then
    echo "error: please install the following programs first: ${not_installed[*]}." >&2
    exit 1
  fi
}

if [[ "${#@}" -lt 1 || -z "${1}" ]]; then
  echo "usage: ${0} version [arch]" >&2
  exit 1
fi

check_programs flatpak find sort perl git

DRIVER_VERSION=${1}
DRIVER_VERSION_DASH=${DRIVER_VERSION//./-}
DRIVER_VERSION_MAJOR=${DRIVER_VERSION%%.*}

# If the [arch] cli option is set to i386, assume that only files from the GL32 extension should be listed.
if [ "${2:-}" == 'i386' ]; then
  DRIVER_ARCH=i386
  DRIVER_ARCH_EXT=x86_64
  EXT_ARCH=GL32
# Otherwise, try to use the [arch] option in cli, or default to x86_64 if empty.
else
  DRIVER_ARCH=${2:-x86_64}
  DRIVER_ARCH_EXT=${2:-x86_64}
  EXT_ARCH='GL*'
fi

matched_refs=()
for ref in $(flatpak list --runtime --columns=ref); do
  if [[ "${ref}" == org.freedesktop.Platform.${EXT_ARCH}.nvidia-${DRIVER_VERSION_DASH}/${DRIVER_ARCH_EXT}/1.4 ]]; then
    matched_refs+=("${ref}")
  fi
done

if [ "${#matched_refs[@]}" -eq 0 ]; then
  echo "error: driver version ${DRIVER_VERSION} (${DRIVER_ARCH}) for flatpak is not installed." >&2
  exit 1
fi

installation_paths=()
for ref in "${matched_refs[@]}"; do
  installation_paths+=("$(flatpak info -l "${ref}")/files")
  gl32_found=false
  if [[ "${ref}" == org.freedesktop.Platform.GL32.nvidia-* ]]; then
    gl32_found=true
  fi
done

if [[ "${DRIVER_ARCH}" == x86_64 && ${gl32_found} == false ]]; then
  echo "warning: the i386 driver (GL32 extension) for driver ${DRIVER_VERSION} (${DRIVER_ARCH}) is not installed." >&2
fi

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/tests"

for installation_path in "${installation_paths[@]}"; do
  if [[ "${installation_path}" == */org.freedesktop.Platform.GL32.nvidia-* ]]; then
    arch=i386
  else
    arch=${DRIVER_ARCH}
  fi
  test_file="${TESTS_DIR}/nvidia-R${DRIVER_VERSION_MAJOR}-file-list-${arch}.txt"

  should_git_add=false
  if [ ! -f "${test_file}" ]; then
    should_git_add=true
  fi

  echo -n "info: generating test file '$(realpath -s --relative-to "${PWD}" "${test_file}")' for driver ${DRIVER_VERSION} (${arch})... "

  cd "${installation_path}"
  # The output format of the test file is documented in the README.md file.
  find . ! -path . ! -path ./.ref ! -path ./manifest.json \
    \( -type l -printf '%P (%Y) -> %l | %#m (%M)\n' -o -printf '%P | %#m (%M)\n' \) |
    perl -pe "s/\Q${DRIVER_VERSION}\E|\Q${DRIVER_VERSION_DASH}\E/${DRIVER_VERSION_MAJOR}/g" |
    LC_ALL=C sort -fVo "${test_file}"
  cd - >/dev/null

  echo 'done'

  if [ "${should_git_add}" == true ]; then
    git add "${test_file}"
  fi
done
