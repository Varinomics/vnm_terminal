#!/bin/sh

set -eu

usage()
{
    echo "usage: $0 --payload PATH --ifw-root PATH [--output PATH]" >&2
    exit 2
}

payload_path=
ifw_root=
output_path=

while [ "$#" -gt 0 ]; do
    case "$1" in
        --payload)
            [ "$#" -ge 2 ] || usage
            payload_path=$2
            shift 2
            ;;
        --ifw-root)
            [ "$#" -ge 2 ] || usage
            ifw_root=$2
            shift 2
            ;;
        --output)
            [ "$#" -ge 2 ] || usage
            output_path=$2
            shift 2
            ;;
        *)
            usage
            ;;
    esac
done

[ -n "$payload_path" ] || usage
[ -n "$ifw_root" ] || usage

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
payload_path=$(CDPATH= cd -- "$payload_path" && pwd)
ifw_root=$(CDPATH= cd -- "$ifw_root" && pwd)
binary_creator="$ifw_root/bin/binarycreator"

[ -x "$binary_creator" ] || {
    echo "Qt IFW binarycreator was not found: $binary_creator" >&2
    exit 1
}
[ -x "$payload_path/usr/bin/vnm_terminal" ] || {
    echo "The payload does not contain usr/bin/vnm_terminal: $payload_path" >&2
    exit 1
}
[ -f "$payload_path/usr/bin/qt.conf" ] || {
    echo "The payload does not contain usr/bin/qt.conf: $payload_path" >&2
    exit 1
}

# The library directory follows CMAKE_INSTALL_LIBDIR, which is
# distribution-specific: Debian derivatives use a multiarch lib/<triple> while
# others use lib64 or a plain lib. The payload is internally consistent with
# whichever one it was installed under, so the staged package keeps the same
# relative directory rather than the one this script happened to be written on.
# An ambiguous payload is refused instead of resolved by picking a match.
payload_libdir=
for candidate in \
    "$payload_path"/usr/lib*/vnm_terminal \
    "$payload_path"/usr/lib*/*/vnm_terminal
do
    [ -d "$candidate" ] || continue
    if [ -n "$payload_libdir" ]; then
        echo "The payload contains more than one vnm_terminal library directory." >&2
        exit 1
    fi
    payload_libdir=$candidate
done
[ -n "$payload_libdir" ] || {
    echo "The payload has no usr/lib*/vnm_terminal directory: $payload_path" >&2
    exit 1
}
package_libdir=${payload_libdir#"$payload_path/usr/"}
package_libdir=${package_libdir%/vnm_terminal}

package_version=$(sed -nE \
    's/^project\(vnm_terminal .* VERSION ([^ )]+)\).*/\1/p' \
    "$repository_root/CMakeLists.txt")
[ -n "$package_version" ] || {
    echo "Could not derive the package version." >&2
    exit 1
}

if [ -z "$output_path" ]; then
    output_path="$repository_root/dist/vnm_terminal_v${package_version}_linux_x64.run"
fi

stage_root="$repository_root/build_ifw/vnm_terminal_linux_installer"
config_root="$stage_root/config"
package_root="$stage_root/packages/com.varinomics.vnm_terminal"
package_data_root="$package_root/data"
package_meta_root="$package_root/meta"
source_root="$repository_root/packaging/linux/ifw"

rm -rf -- "$stage_root"
mkdir -p "$config_root" "$package_data_root" "$package_meta_root"

cp -a "$payload_path/usr/bin" "$package_data_root/"
mkdir -p "$package_data_root/$package_libdir" "$package_data_root/share/icons"
cp -a "$payload_libdir" "$package_data_root/$package_libdir/"
cp -a "$repository_root/LICENSE" \
    "$repository_root/THIRD_PARTY_NOTICES.md" \
    "$package_data_root/"
cp -a "$repository_root/packaging/linux/com.varinomics.vnm-terminal.png" \
    "$package_data_root/share/icons/"

sed "s/@VNM_TERMINAL_VERSION@/$package_version/g" \
    "$source_root/config.xml.in" > "$config_root/config.xml"
sed \
    -e "s/@VNM_TERMINAL_VERSION@/$package_version/g" \
    -e "s/@VNM_TERMINAL_RELEASE_DATE@/$(date +%Y-%m-%d)/g" \
    "$source_root/package.xml.in" > "$package_meta_root/package.xml"

cp -a "$source_root/controller.qs" "$config_root/"
cp -a "$source_root/theme_resources.qrc" "$config_root/"
cp -a "$source_root/installscript.qs" "$package_meta_root/"
cp -a "$repository_root/LICENSE" "$package_meta_root/LICENSE.txt"
cp -a "$repository_root/packaging/windows/ifw/style.qss" "$config_root/"
cp -a "$repository_root/packaging/windows/ifw/varinomics_banner.png" \
    "$config_root/"
cp -a \
    "$repository_root/packaging/windows/ifw/checkbox_check.svg" \
    "$repository_root/packaging/windows/ifw/combo_arrow.svg" \
    "$repository_root/packaging/windows/ifw/radio_dot.svg" \
    "$repository_root/packaging/windows/ifw/varinomics_geometry.png" \
    "$config_root/"
cp -a "$repository_root/packaging/linux/com.varinomics.vnm-terminal.png" \
    "$config_root/"

mkdir -p "$(dirname -- "$output_path")"
"$binary_creator" --offline-only \
    --config "$config_root/config.xml" \
    --packages "$stage_root/packages" \
    --resources "$config_root/theme_resources.qrc" \
    "$output_path"
chmod 0755 "$output_path"
(CDPATH= cd -- "$(dirname -- "$output_path")" && \
    sha256sum "$(basename -- "$output_path")" \
        > "$(basename -- "$output_path").sha256")

echo "Created $output_path"
