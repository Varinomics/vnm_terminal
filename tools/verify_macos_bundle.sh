#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: verify_macos_bundle.sh <app-bundle> <Mach-O-architecture>" >&2
    exit 1
fi

app_bundle="$1"
architecture="$2"
executable="${app_bundle}/Contents/MacOS/vnm_terminal"
test -x "${executable}"

actual_architecture="$(lipo -archs "${executable}")"
if [[ "${actual_architecture}" != "${architecture}" ]]; then
    echo "Expected a native ${architecture} application, found ${actual_architecture}: ${executable}" >&2
    exit 1
fi

# Qt's vendor frameworks may be universal. Every runtime component must still
# contain the package's architecture, including plugins in nested directories.
while IFS= read -r -d '' component; do
    description="$(file -b "${component}")"
    if [[ "${description}" == *Mach-O* ]]; then
        lipo -verify_arch "${architecture}" "${component}"
        echo "${component}: $(lipo -archs "${component}")"
    fi
done < <(find "${app_bundle}" -type f -print0)

# Controlled code is linked into the executable. Vendor Qt binaries retain
# their supplied layout; standalone debugging and relinking files do not ship.
load_commands="$(otool -l "${executable}")"
symbols="$(nm -ap "${executable}")"
if [[ "${load_commands}" == *__DWARF* ]] ||
    awk '$2 == "-" { found = 1 } END { exit !found }' <<< "${symbols}"
then
    echo "Debug information is present in ${executable}." >&2
    exit 1
fi

while IFS= read -r -d '' component; do
    case "${component}" in
        *.dSYM|*.pdb|*.debug|*.a|*.o|*.obj)
            echo "Debug or relinking material is present: ${component}" >&2
            exit 1
            ;;
    esac
done < <(find "${app_bundle}" -print0)

echo "Verified macOS ${architecture} bundle: ${app_bundle}"
