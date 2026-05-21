#!/bin/sh

inventory_relative_path=docs/terminal_reference_inventory.md
reference_id=libvterm
force=false
output_directory=
temp_path_to_clean=

cleanup_temp_file()
{
    if [ -n "$temp_path_to_clean" ]; then
        rm -f "$temp_path_to_clean"
    fi
}

trap cleanup_temp_file EXIT
trap 'cleanup_temp_file; exit 1' HUP INT TERM

usage()
{
    cat <<EOF
usage: $0 [options] [output-dir]

Fetch, verify, build, and locally install the libvterm release pinned in
docs/terminal_reference_inventory.md.

Options:
  -f, --force              Rebuild managed artifacts; reuse a verified tarball.
  -o, --output-dir DIR     Write managed artifacts under DIR.
  -h, --help               Show this help.

The default output directory is build/conformance/libvterm from the repository
root. Existing managed artifacts are validated and reused. Incomplete managed
artifacts stop the script; rerun with --force to replace them.

Required build tools: POSIX sh, awk, sed, tar, a C compiler, GNU make
(make on Linux/WSL or gmake on FreeBSD), libtool, install, mktemp, and
curl/fetch/wget.
EOF
}

die()
{
    printf '%s\n' "ERROR: $*" >&2
    exit 1
}

have_command()
{
    command -v "$1" >/dev/null 2>&1
}

require_command()
{
    if ! have_command "$1"; then
        die "$1 is required"
    fi
}

make_temp_file()
{
    temp_dir=$1
    temp_name=$2

    require_command mktemp
    temp_path_to_clean=$(mktemp "$temp_dir/.$temp_name.XXXXXX") ||
        die "could not create temporary file in $temp_dir"
}

script_directory()
{
    case $0 in
        */*) script_path=$0 ;;
        *)
            script_path=$(command -v "$0") || return 1
            ;;
    esac

    script_dir=$(dirname "$script_path") || return 1
    CDPATH= cd "$script_dir" 2>/dev/null && pwd -P
}

repository_root()
{
    script_dir=$(script_directory) || return 1
    CDPATH= cd "$script_dir/../.." 2>/dev/null && pwd -P
}

absolute_output_path()
{
    path=$1

    if [ -z "$path" ]; then
        root=$(repository_root) || die "could not resolve repository root"
        printf '%s\n' "$root/build/conformance/libvterm"
        return
    fi

    case $path in
        /*) printf '%s\n' "$path" ;;
        *)
            cwd=$(pwd -P) || die "could not resolve current directory"
            printf '%s\n' "$cwd/$path"
            ;;
    esac
}

canonical_path_for_policy()
{
    path=$1
    existing_path=$path
    suffix=

    while [ ! -e "$existing_path" ]; do
        path_name=$(basename "$existing_path") || return 1
        path_parent=$(dirname "$existing_path") || return 1
        if [ "$path_parent" = "$existing_path" ]; then
            return 1
        fi

        suffix=/$path_name$suffix
        existing_path=$path_parent
    done

    if [ -d "$existing_path" ]; then
        existing_real=$(CDPATH= cd "$existing_path" 2>/dev/null && pwd -P) ||
            return 1
        printf '%s%s\n' "$existing_real" "$suffix"
        return 0
    fi

    path_name=$(basename "$existing_path") || return 1
    path_parent=$(dirname "$existing_path") || return 1
    path_parent_real=$(CDPATH= cd "$path_parent" 2>/dev/null && pwd -P) ||
        return 1
    printf '%s/%s%s\n' "$path_parent_real" "$path_name" "$suffix"
}

validate_output_path_policy()
{
    path=$1
    repo_root=$(repository_root) || die "could not resolve repository root"
    repo_build_dir=$repo_root/build
    policy_path=$(canonical_path_for_policy "$path") ||
        die "could not resolve output path for policy check: $path"

    case $policy_path in
        "$repo_root"|"$repo_root"/*)
            case $policy_path in
                "$repo_build_dir"|"$repo_build_dir"/*) ;;
                *)
                    die "refusing to place managed libvterm artifacts inside the repository outside build/: $path"
                    ;;
            esac
            ;;
    esac
}

ensure_directory()
{
    path=$1

    if [ -e "$path" ] && [ ! -d "$path" ]; then
        die "path exists but is not a directory: $path"
    fi

    mkdir -p "$path" || die "could not create directory: $path"
}

inventory_value()
{
    key=$1

    awk -v reference_id="$reference_id" -v key="$key" '
        $0 == "## " reference_id {
            in_section = 1
            next
        }
        in_section && /^## / {
            exit
        }
        in_section && index($0, key ":") == 1 {
            sub("^[^:]*:[[:space:]]*", "", $0)
            print
            found = 1
            exit
        }
        END {
            if (!found) {
                exit 1
            }
        }
    ' "$inventory_file"
}

is_sha256_hex()
{
    value=$1
    length=$(printf '%s' "$value" | wc -c | tr -d '[:space:]') ||
        return 1

    if [ "$length" != 64 ]; then
        return 1
    fi

    case $value in
        *[!0123456789abcdef]*|'')
            return 1
            ;;
    esac

    return 0
}

validate_tarball_file_name()
{
    name=$1

    case $name in
        ''|.|..)
            die "libvterm version_pin tarball name must be a basename-only .tar.gz file"
            ;;
        */*|*'\'*)
            die "libvterm version_pin tarball name must not contain path separators: $name"
            ;;
        *.tar.gz)
            ;;
        *)
            die "libvterm version_pin is not a .tar.gz tarball: $name"
            ;;
    esac
}

load_libvterm_pin()
{
    root=$(repository_root) || die "could not resolve repository root"
    inventory_file=$root/$inventory_relative_path

    if [ ! -f "$inventory_file" ]; then
        die "reference inventory does not exist: $inventory_file"
    fi

    source_url=$(inventory_value source_url) ||
        die "reference inventory is missing libvterm source_url"
    version_pin=$(inventory_value version_pin) ||
        die "reference inventory is missing libvterm version_pin"

    tarball_file_name=$(printf '%s\n' "$version_pin" |
        awk -F',' '{ value = $1; gsub(/^[[:space:]]+|[[:space:]]+$/, "", value); print value; exit }') ||
        die "could not parse libvterm version_pin"
    validate_tarball_file_name "$tarball_file_name"

    expected_sha256=$(printf '%s\n' "$version_pin" |
        awk '
            {
                for (i = 1; i < NF; ++i) {
                    if ($i == "sha256") {
                        print tolower($(i + 1))
                        found = 1
                        exit
                    }
                }
            }
            END {
                if (!found) {
                    exit 1
                }
            }
        ') || die "could not parse libvterm sha256 from version_pin"

    if ! is_sha256_hex "$expected_sha256"; then
        die "libvterm version_pin sha256 is not a 64-character hex digest"
    fi

    case $source_url in
        */) tarball_url=$source_url$tarball_file_name ;;
        *) tarball_url=$source_url/$tarball_file_name ;;
    esac

    release_dir_name=${tarball_file_name%.tar.gz}
}

sha256_file()
{
    path=$1

    if have_command sha256sum; then
        hash_output=$(sha256sum "$path") || return 1
        hash_value=$(printf '%s\n' "$hash_output" |
            awk 'NF { print tolower($1); exit }') || return 1
        if [ -z "$hash_value" ]; then
            return 1
        fi
        printf '%s\n' "$hash_value"
        return
    fi

    if have_command sha256; then
        if hash_output=$(sha256 -q "$path" 2>/dev/null); then
            hash_value=$(printf '%s\n' "$hash_output" |
                awk 'NF { print tolower($1); exit }') || return 1
        else
            hash_output=$(sha256 "$path") || return 1
            hash_value=$(printf '%s\n' "$hash_output" |
                awk 'NF { print tolower($NF); exit }') || return 1
        fi
        if [ -z "$hash_value" ]; then
            return 1
        fi
        printf '%s\n' "$hash_value"
        return
    fi

    if have_command shasum; then
        hash_output=$(shasum -a 256 "$path") || return 1
        hash_value=$(printf '%s\n' "$hash_output" |
            awk 'NF { print tolower($1); exit }') || return 1
        if [ -z "$hash_value" ]; then
            return 1
        fi
        printf '%s\n' "$hash_value"
        return
    fi

    printf '%s\n' "ERROR: no SHA-256 tool found; install sha256sum, sha256, or shasum" >&2
    return 1
}

verify_tarball()
{
    actual_sha256=$(sha256_file "$tarball_path") ||
        die "could not compute SHA-256 for $tarball_path"

    if [ "$actual_sha256" != "$expected_sha256" ]; then
        die "SHA-256 mismatch for $tarball_path: expected $expected_sha256, got $actual_sha256"
    fi
}

validate_archive_members()
{
    make_temp_file "$source_root" archive-members
    archive_members_path=$temp_path_to_clean

    printf '%s\n' "__VNM_ARCHIVE_MEMBER_TYPES__" > "$archive_members_path" ||
        die "could not write archive member list: $archive_members_path"
    tar -tvzf "$tarball_path" >> "$archive_members_path" ||
        die "could not list archive member metadata in $tarball_path"
    printf '%s\n' "__VNM_ARCHIVE_MEMBER_NAMES__" >> "$archive_members_path" ||
        die "could not write archive member list: $archive_members_path"
    tar -tzf "$tarball_path" >> "$archive_members_path" ||
        die "could not list archive members in $tarball_path"

    awk -v release_dir_name="$release_dir_name" '
        function reject(member, reason)
        {
            printf "ERROR: unsafe archive member: %s (%s)\n", member, reason > "/dev/stderr"
            failed = 1
        }

        function archive_type_name(entry_type)
        {
            if (entry_type == "l") {
                return "symlink"
            }
            if (entry_type == "h") {
                return "hardlink"
            }
            if (entry_type == "b") {
                return "block device"
            }
            if (entry_type == "c") {
                return "character device"
            }
            if (entry_type == "p") {
                return "FIFO"
            }
            if (entry_type == "") {
                return "missing type"
            }
            return "non-regular/non-directory entry type " entry_type
        }

        function validate_member_type(member, entry_type)
        {
            if (entry_type == "-" || entry_type == "d") {
                return
            }

            reject(member, archive_type_name(entry_type))
        }

        function validate_member_path(member, entry_type, component_count, i)
        {
            if (member == "") {
                reject("<empty>", "empty path")
                return
            }

            if (substr(member, 1, 1) == "/") {
                reject(member, "absolute path")
                return
            }

            component_count = split(member, components, "/")
            for (i = 1; i <= component_count; ++i) {
                if (components[i] == "") {
                    if (i == component_count && entry_type == "d") {
                        continue
                    }
                    reject(member, "empty path component")
                    return
                }

                if (components[i] == ".") {
                    reject(member, "contains . path component")
                    return
                }

                if (components[i] == "..") {
                    reject(member, "contains .. path component")
                    return
                }
            }

            if (components[1] != release_dir_name) {
                reject(member, "unexpected top-level prefix")
            }
        }

        $0 == "__VNM_ARCHIVE_MEMBER_TYPES__" {
            section = "types"
            next
        }

        $0 == "__VNM_ARCHIVE_MEMBER_NAMES__" {
            section = "names"
            next
        }

        section == "types" {
            types[++type_count] = substr($0, 1, 1)
            next
        }

        section == "names" {
            members[++member_count] = $0
            next
        }

        {
            printf "ERROR: internal archive member list format is invalid\n" > "/dev/stderr"
            failed = 1
        }

        END {
            if (member_count == 0) {
                printf "ERROR: archive has no members\n" > "/dev/stderr"
                failed = 1
            }

            if (type_count != member_count) {
                printf "ERROR: archive member metadata count does not match path count\n" > "/dev/stderr"
                failed = 1
            }

            for (entry_index = 1; entry_index <= member_count && entry_index <= type_count; ++entry_index) {
                validate_member_type(members[entry_index], types[entry_index])
                validate_member_path(members[entry_index], types[entry_index])
            }

            exit failed ? 1 : 0
        }
    ' "$archive_members_path" || die "tarball contains unsafe archive members"

    rm -f "$archive_members_path" ||
        die "could not remove temporary archive member list: $archive_members_path"
    temp_path_to_clean=
}

download_file()
{
    url=$1
    destination=$2
    destination_dir=$(dirname "$destination") || die "invalid destination: $destination"
    destination_name=$(basename "$destination") || die "invalid destination: $destination"

    ensure_directory "$destination_dir"

    if [ -e "$destination" ]; then
        die "download destination already exists: $destination"
    fi

    make_temp_file "$destination_dir" "$destination_name"
    temp_path=$temp_path_to_clean
    printf '%s\n' "download: $url"

    if have_command curl; then
        curl -fL --connect-timeout 30 --max-time 180 -o "$temp_path" "$url" ||
            die "failed to download $url"
    elif have_command fetch; then
        fetch -T 30 -o "$temp_path" "$url" ||
            die "failed to download $url"
    elif have_command wget; then
        wget -T 30 -t 3 -O "$temp_path" "$url" ||
            die "failed to download $url"
    else
        die "no download tool found; install curl, fetch, or wget"
    fi

    if [ ! -s "$temp_path" ]; then
        die "download produced an empty file: $url"
    fi

    mv "$temp_path" "$destination" ||
        die "could not publish downloaded file to $destination"
    temp_path_to_clean=
}

is_gnu_make()
{
    "$1" --version 2>/dev/null |
        awk 'NR == 1 && /GNU Make/ { found = 1 } END { exit found ? 0 : 1 }'
}

select_gnu_make()
{
    if [ -n "${MAKE:-}" ]; then
        case $MAKE in
            *[[:space:]]*) die "MAKE must name one GNU make executable, not include arguments" ;;
        esac
        if ! have_command "$MAKE"; then
            die "MAKE executable was not found: $MAKE"
        fi
        if is_gnu_make "$MAKE"; then
            printf '%s\n' "$MAKE"
            return
        fi
        die "MAKE is not GNU make: $MAKE"
    fi

    if have_command gmake && is_gnu_make gmake; then
        printf '%s\n' gmake
        return
    fi

    if have_command make && is_gnu_make make; then
        printf '%s\n' make
        return
    fi

    die "GNU make is required; install gmake on FreeBSD or GNU make on Linux/WSL"
}

require_build_tools()
{
    require_command awk
    require_command sed
    require_command tar
    require_command install
    require_command libtool
    require_command mktemp

    if [ -n "${CC:-}" ]; then
        case $CC in
            *[[:space:]]*) ;;
            *)
                if ! have_command "$CC"; then
                    die "CC executable was not found: $CC"
                fi
                ;;
        esac
    else
        require_command cc
    fi

    make_command=$(select_gnu_make)
}

remove_path_if_present()
{
    path=$1

    if [ -z "$path" ]; then
        die "internal error: empty path removal requested"
    fi

    if [ -e "$path" ]; then
        rm -rf "$path" || die "could not remove $path"
    fi
}

remove_managed_artifacts()
{
    remove_path_if_present "$source_dir"
    remove_path_if_present "$install_prefix"

    if [ -e "$tarball_path" ]; then
        if [ -f "$tarball_path" ]; then
            actual_sha256=$(sha256_file "$tarball_path" 2>/dev/null) || actual_sha256=
            if [ "$actual_sha256" = "$expected_sha256" ]; then
                printf '%s\n' "preserve verified libvterm tarball: $tarball_path"
                return
            fi
        fi

        remove_path_if_present "$tarball_path"
    fi
}

find_libvterm_library()
{
    lib_dir=$1

    for candidate in \
        "$lib_dir/libvterm.a" \
        "$lib_dir/libvterm.so" \
        "$lib_dir"/libvterm.so.[0-9]* \
        "$lib_dir/libvterm.dylib"
    do
        if [ -s "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    return 1
}

manifest_value()
{
    key=$1

    awk -F= -v key="$key" '
        $1 == key {
            sub("^[^=]*=", "", $0)
            print
            found = 1
            exit
        }
        END {
            if (!found) {
                exit 1
            }
        }
    ' "$manifest_path"
}

write_install_manifest()
{
    ensure_directory "$install_prefix"
    make_temp_file "$install_prefix" libvterm.manifest
    manifest_temp_path=$temp_path_to_clean

    {
        printf 'tarball_file_name=%s\n' "$tarball_file_name"
        printf 'expected_sha256=%s\n' "$expected_sha256"
    } > "$manifest_temp_path" || die "could not write install manifest: $manifest_temp_path"

    mv "$manifest_temp_path" "$manifest_path" ||
        die "could not publish install manifest: $manifest_path"
    temp_path_to_clean=
}

validate_install_manifest()
{
    if [ ! -s "$manifest_path" ]; then
        die "managed libvterm install is missing its manifest under $install_prefix"
    fi

    manifest_tarball_file_name=$(manifest_value tarball_file_name) ||
        die "managed libvterm manifest is missing tarball_file_name: $manifest_path"
    manifest_expected_sha256=$(manifest_value expected_sha256) ||
        die "managed libvterm manifest is missing expected_sha256: $manifest_path"

    if [ "$manifest_tarball_file_name" != "$tarball_file_name" ]; then
        die "managed libvterm manifest tarball does not match current pin: $manifest_path"
    fi

    if [ "$manifest_expected_sha256" != "$expected_sha256" ]; then
        die "managed libvterm manifest SHA-256 does not match current pin: $manifest_path"
    fi
}

validate_existing_install()
{
    if [ ! -d "$install_prefix" ]; then
        return 1
    fi

    validate_install_manifest

    if [ ! -f "$tarball_path" ]; then
        die "managed libvterm install exists but the verified tarball is missing: $tarball_path"
    fi

    verify_tarball

    if [ ! -s "$install_prefix/include/vterm.h" ]; then
        die "managed libvterm install is missing non-empty include/vterm.h under $install_prefix"
    fi

    library_path=$(find_libvterm_library "$install_prefix/lib") ||
        die "managed libvterm install is missing non-empty libvterm library under $install_prefix/lib"

    return 0
}

extract_source()
{
    ensure_directory "$source_root"

    if [ -e "$source_dir" ]; then
        die "source directory already exists; rerun with --force: $source_dir"
    fi

    validate_archive_members

    tar -xzf "$tarball_path" -C "$source_root" ||
        die "could not unpack $tarball_path"

    if [ ! -f "$source_dir/Makefile" ]; then
        die "tarball did not contain expected source directory: $release_dir_name"
    fi
}

build_and_install()
{
    printf '%s\n' "build: $source_dir"
    (
        CDPATH= cd "$source_dir" &&
            "$make_command" PREFIX="$install_prefix" libvterm.la
    ) || die "failed to build libvterm"

    printf '%s\n' "install: $install_prefix"
    (
        CDPATH= cd "$source_dir" &&
            "$make_command" PREFIX="$install_prefix" install-inc install-lib
    ) || die "failed to install libvterm headers and library"

    write_install_manifest
}

print_cmake_flags()
{
    repo_root=$(repository_root) || die "could not resolve repository root"
    include_dir=$install_prefix/include
    corpus_dir=$repo_root/tests/conformance/captures
    library_path=$(find_libvterm_library "$install_prefix/lib") ||
        die "installed libvterm library was not found under $install_prefix/lib"

    printf '%s\n' "libvterm install: $install_prefix"
    printf '%s\n' "CMake flags:"
    printf '  -DVNM_TERMINAL_LIBVTERM_INCLUDE_DIR=%s\n' "$include_dir"
    printf '  -DVNM_TERMINAL_LIBVTERM_LIBRARY=%s\n' "$library_path"
    printf '  -DVNM_TERMINAL_LIBVTERM_CORPUS_DIR=%s\n' "$corpus_dir"
}

while [ "$#" -gt 0 ]; do
    case $1 in
        -f|--force)
            force=true
            shift
            ;;
        -o|--output-dir)
            [ "$#" -ge 2 ] || die "$1 requires a directory argument"
            [ -z "$output_directory" ] || die "output directory was specified more than once"
            output_directory=$2
            shift 2
            ;;
        --output-dir=*)
            [ -z "$output_directory" ] || die "output directory was specified more than once"
            output_directory=${1#--output-dir=}
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        -*)
            die "unknown option: $1"
            ;;
        *)
            [ -z "$output_directory" ] || die "output directory was specified more than once"
            output_directory=$1
            shift
            ;;
    esac
done

if [ "$#" -gt 0 ]; then
    [ -z "$output_directory" ] || die "output directory was specified more than once"
    output_directory=$1
    shift
fi

if [ "$#" -gt 0 ]; then
    die "too many arguments"
fi

load_libvterm_pin
output_path=$(absolute_output_path "$output_directory")
validate_output_path_policy "$output_path"

case $output_path in
    /) die "refusing to use / as the output directory" ;;
    *[[:space:]]*)
        die "output directory must not contain whitespace; libvterm's Makefile does not quote install paths"
        ;;
esac

downloads_dir=$output_path/downloads
source_root=$output_path/source
source_dir=$source_root/$release_dir_name
install_prefix=$output_path/install
tarball_path=$downloads_dir/$tarball_file_name
manifest_path=$install_prefix/libvterm.manifest

printf '%s\n' "libvterm target: $output_path"
printf '%s\n' "libvterm pin: $tarball_file_name"
printf '%s\n' "expected sha256: $expected_sha256"

if [ "$force" = true ]; then
    remove_managed_artifacts
fi

if validate_existing_install; then
    printf '%s\n' "existing libvterm install validated"
    print_cmake_flags
    exit 0
fi

if [ -e "$install_prefix" ]; then
    die "managed libvterm install is incomplete; rerun with --force: $install_prefix"
fi

if [ -e "$source_dir" ]; then
    die "managed libvterm source directory already exists; rerun with --force: $source_dir"
fi

if [ -e "$tarball_path" ]; then
    if [ ! -f "$tarball_path" ]; then
        die "managed libvterm tarball path exists but is not a file: $tarball_path"
    fi
    verify_tarball
    printf '%s\n' "existing libvterm tarball validated: $tarball_path"
else
    download_file "$tarball_url" "$tarball_path"
    verify_tarball
fi

require_build_tools
extract_source
build_and_install
validate_existing_install
print_cmake_flags
