#!/usr/bin/env bash

set -euo pipefail

check_program() {
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

if [ "${#@}" -lt 1 ]; then
  echo "usage: ${0} version [arch]" >&2
  exit 1
fi

if [ -z "${1}" ]; then
  echo 'error: you must specify a driver version.' >&2
  exit 1
fi

DRIVER_VERSION=${1}
DRIVER_ARCH=${2:-x86_64}
DRIVER_VERSION_DASH=${DRIVER_VERSION//./-}
DRIVER_VERSION_MAJOR=${DRIVER_VERSION%%.*}

check_program flatpak find sort perl

matched_refs=()
for ref in $(flatpak list --runtime --columns=ref); do
  if [[ "${ref}" == org.freedesktop.Platform.GL*.nvidia-${DRIVER_VERSION_DASH}/${DRIVER_ARCH}/1.4 ]]; then
    matched_refs+=("${ref}")
  fi
done

if [ "${#matched_refs[@]}" -eq 0 ]; then
  echo "error: driver version ${DRIVER_VERSION} (${DRIVER_ARCH}) for flatpak is not installed. test file can't be generated." >&2
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

if [[ "${DRIVER_ARCH}" == x86_64 ]] && [[ ${gl32_found} == false ]]; then
  echo "warning: the i386 driver (GL32 extension) for driver ${DRIVER_VERSION} (${DRIVER_ARCH}) is not installed!" >&2
fi

for installation_path in "${installation_paths[@]}"; do
  if [[ "${installation_path}" == */org.freedesktop.Platform.GL32.nvidia-* ]]; then
    arch=i386
  else
    arch=${DRIVER_ARCH}
  fi
  test_file="$(printf './tests/nvidia-R%s-file-list-%s.txt' "${DRIVER_VERSION_MAJOR}" "${arch}")"
  echo -n "info: generating test file '${test_file}' for driver ${DRIVER_VERSION} (${arch})... "
  (
    cd "${installation_path}" &&
      find . ! -path . ! -path ./.ref ! -path ./manifest.json \
        \( -type l -printf '%P (%Y) -> %l | %#m (%M)\n' -o -printf '%P | %#m (%M)\n' \)
  ) |
    perl -pe "s/\Q${DRIVER_VERSION}\E|\Q${DRIVER_VERSION_DASH}\E/${DRIVER_VERSION_MAJOR}/g" |
    LC_ALL=C sort -fVo "${test_file}"
  echo 'done'
done
