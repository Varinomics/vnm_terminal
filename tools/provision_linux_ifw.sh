#!/bin/sh

set -eu

usage()
{
    echo "usage: $0 --destination PATH" >&2
    exit 2
}

destination_path=

while [ "$#" -gt 0 ]; do
    case "$1" in
        --destination)
            [ "$#" -ge 2 ] || usage
            destination_path=$2
            shift 2
            ;;
        *)
            usage
            ;;
    esac
done

[ -n "$destination_path" ] || usage

ifw_version=4.7.0
archive_name=4.7.0-0-202402150941ifw-linux-x64.7z
archive_url="https://download.qt.io/online/qtsdkrepository/linux_x64/desktop/tools_ifw/qt.tools.ifw.47/$archive_name"
archive_sha256=dd86e6098e88baf0aad28a03334b505f8767eb45294b74d118d16a4a1d64f035
work_root=$(mktemp -d)
archive_path="$work_root/$archive_name"
extract_root="$work_root/extract"

cleanup()
{
    rm -rf -- "$work_root"
}
trap cleanup EXIT HUP INT TERM

curl --fail --location --retry 3 "$archive_url" --output "$archive_path"
actual_sha256=$(sha256sum "$archive_path" | cut -d ' ' -f 1)
if [ "$actual_sha256" != "$archive_sha256" ]; then
    echo "Qt IFW archive checksum mismatch." >&2
    echo "Expected: $archive_sha256" >&2
    echo "Actual:   $actual_sha256" >&2
    exit 1
fi

mkdir -p "$extract_root"
(CDPATH= cd -- "$extract_root" && cmake -E tar xf "$archive_path")
extracted_root="$extract_root/Tools/QtInstallerFramework/4.7"
[ -x "$extracted_root/bin/binarycreator" ] || {
    echo "The Qt IFW archive does not contain binarycreator." >&2
    exit 1
}

version_output=$($extracted_root/bin/installerbase --version 2>&1)
case "$version_output" in
    *"IFW Version: $ifw_version,"*) ;;
    *)
        echo "The Qt IFW archive is not version $ifw_version." >&2
        exit 1
        ;;
esac

rm -rf -- "$destination_path"
mkdir -p "$(dirname -- "$destination_path")"
mv "$extracted_root" "$destination_path"
echo "Qt Installer Framework $ifw_version provisioned at $destination_path"
