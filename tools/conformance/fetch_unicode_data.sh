#!/bin/sh

unicode_version=16.0.0
emoji_version=16.0
manifest_file_name=vnm_terminal_unicode_data_manifest.json
artifact_kind=vnm_terminal_unicode_conformance_data
generated_by=tools/conformance/fetch_unicode_data.sh
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

Fetch Unicode conformance data for vnm_terminal.

Options:
  -f, --force              Redownload and replace existing data artifacts.
  -o, --output-dir DIR     Write data under DIR.
  -h, --help               Show this help.

The default output directory is build/unicode-${unicode_version} from the
repository root. Existing Unicode data is not overwritten unless --force is
passed; when existing data is present, the manifest is validated instead.
EOF
}

die()
{
    printf '%s\n' "ERROR: $*" >&2
    exit 1
}

source_files()
{
    cat <<EOF
EastAsianWidth.txt|https://www.unicode.org/Public/${unicode_version}/ucd/EastAsianWidth.txt
UnicodeData.txt|https://www.unicode.org/Public/${unicode_version}/ucd/UnicodeData.txt
PropList.txt|https://www.unicode.org/Public/${unicode_version}/ucd/PropList.txt
auxiliary/GraphemeBreakTest.txt|https://www.unicode.org/Public/${unicode_version}/ucd/auxiliary/GraphemeBreakTest.txt
emoji/emoji-data.txt|https://www.unicode.org/Public/${unicode_version}/ucd/emoji/emoji-data.txt
emoji/emoji-variation-sequences.txt|https://www.unicode.org/Public/${unicode_version}/ucd/emoji/emoji-variation-sequences.txt
emoji/emoji-zwj-sequences.txt|https://www.unicode.org/Public/emoji/${emoji_version}/emoji-zwj-sequences.txt
EOF
}

source_file_count()
{
    source_files | wc -l | tr -d '[:space:]'
}

have_command()
{
    command -v "$1" >/dev/null 2>&1
}

make_temp_file()
{
    temp_dir=$1
    temp_name=$2

    if ! have_command mktemp; then
        die "mktemp is required to create temporary files"
    fi

    temp_path_to_clean=$(mktemp "$temp_dir/.$temp_name.XXXXXX") ||
        die "could not create temporary file in $temp_dir"
}

publish_file()
{
    source_path=$1
    destination=$2

    if [ "$force" = true ]; then
        mv -f "$source_path" "$destination" ||
            die "could not replace $destination"
    else
        if [ -e "$destination" ]; then
            die "destination appeared before publish; rerun with --force: $destination"
        fi

        if ! ln "$source_path" "$destination"; then
            if [ -e "$destination" ]; then
                die "destination appeared before publish; rerun with --force: $destination"
            fi
            die "could not write $destination"
        fi

        rm -f "$source_path" ||
            die "could not remove temporary file after publishing $destination"
    fi

    temp_path_to_clean=
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
        printf '%s\n' "$root/build/unicode-$unicode_version"
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

ensure_directory()
{
    path=$1

    if [ -e "$path" ] && [ ! -d "$path" ]; then
        die "path exists but is not a directory: $path"
    fi

    mkdir -p "$path" || die "could not create directory: $path"
}

manifest_path()
{
    printf '%s/%s\n' "$1" "$manifest_file_name"
}

artifact_path()
{
    printf '%s/%s\n' "$1" "$2"
}

has_existing_unicode_artifact()
{
    output_path=$1

    if [ -e "$(manifest_path "$output_path")" ]; then
        return 0
    fi

    while IFS='|' read -r relative_path url; do
        [ -n "$relative_path" ] || continue
        if [ -e "$(artifact_path "$output_path" "$relative_path")" ]; then
            return 0
        fi
    done <<EOF
$(source_files)
EOF

    return 1
}

download_file()
{
    url=$1
    destination=$2
    destination_dir=$(dirname "$destination") || die "invalid destination: $destination"
    destination_name=$(basename "$destination") || die "invalid destination: $destination"

    ensure_directory "$destination_dir"

    if [ -e "$destination" ]; then
        if [ ! -f "$destination" ]; then
            die "download destination exists but is not a file: $destination"
        fi
        if [ "$force" != true ]; then
            die "download destination already exists; rerun with --force: $destination"
        fi
    fi

    make_temp_file "$destination_dir" "$destination_name"
    temp_path=$temp_path_to_clean
    printf '%s\n' "download: $url"

    if have_command curl; then
        curl -fL --connect-timeout 30 --max-time 90 -o "$temp_path" "$url" ||
            die "failed to download $url"
    elif have_command fetch; then
        fetch -o "$temp_path" "$url" ||
            die "failed to download $url"
    elif have_command wget; then
        wget -O "$temp_path" "$url" ||
            die "failed to download $url"
    else
        die "no download tool found; install curl, fetch, or wget"
    fi

    if [ ! -s "$temp_path" ]; then
        die "download produced an empty file: $url"
    fi

    publish_file "$temp_path" "$destination"
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

file_size_bytes()
{
    byte_count=$(wc -c < "$1") || return 1
    byte_count=$(printf '%s\n' "$byte_count" | tr -d '[:space:]') ||
        return 1

    case $byte_count in
        ''|*[!0123456789]*)
            return 1
            ;;
    esac

    printf '%s\n' "$byte_count"
}

manifest_string_value()
{
    manifest=$1
    key=$2

    awk -v key="$key" '
        function extract_string(line) {
            sub("^[^\"]*\"" key "\"[[:space:]]*:[[:space:]]*\"", "", line)
            sub("\"[[:space:]]*,?[[:space:]]*$", "", line)
            return line
        }
        $0 ~ "\"" key "\"[[:space:]]*:" {
            print extract_string($0)
            found = 1
            exit
        }
        END {
            if (!found) {
                exit 1
            }
        }
    ' "$manifest"
}

manifest_file_value()
{
    manifest=$1
    relative_path=$2
    key=$3

    awk -v relative_path="$relative_path" -v key="$key" '
        function string_value(line, field_key) {
            sub("^[^\"]*\"" field_key "\"[[:space:]]*:[[:space:]]*\"", "", line)
            sub("\"[[:space:]]*,?[[:space:]]*$", "", line)
            return line
        }
        function number_value(line, field_key) {
            sub("^[^\"]*\"" field_key "\"[[:space:]]*:[[:space:]]*", "", line)
            sub("[[:space:]]*,?[[:space:]]*$", "", line)
            return line
        }
        /^[[:space:]]*\{[[:space:]]*$/ {
            in_object = 1
            in_entry = 0
            next
        }
        in_object && $0 ~ "\"relative_path\"[[:space:]]*:" {
            value = string_value($0, "relative_path")
            if (value == relative_path) {
                in_entry = 1
                if (key == "relative_path") {
                    print value
                    found = 1
                    exit
                }
            }
            next
        }
        in_entry && key != "bytes" && $0 ~ "\"" key "\"[[:space:]]*:" {
            print string_value($0, key)
            found = 1
            exit
        }
        in_entry && key == "bytes" && $0 ~ "\"bytes\"[[:space:]]*:" {
            print number_value($0, key)
            found = 1
            exit
        }
        in_object && /^[[:space:]]*\}[,]?[[:space:]]*$/ {
            in_object = 0
            in_entry = 0
            next
        }
        END {
            if (!found) {
                exit 1
            }
        }
    ' "$manifest"
}

require_manifest_string()
{
    manifest=$1
    key=$2
    expected=$3
    value=$(manifest_string_value "$manifest" "$key") ||
        die "manifest is missing property $key"

    if [ "$value" != "$expected" ]; then
        die "manifest property $key is '$value', expected '$expected'"
    fi
}

require_manifest_file_value()
{
    manifest=$1
    relative_path=$2
    key=$3
    expected=$4
    value=$(manifest_file_value "$manifest" "$relative_path" "$key") ||
        die "manifest entry $relative_path is missing property $key"

    if [ "$value" != "$expected" ]; then
        die "manifest entry $relative_path property $key is '$value', expected '$expected'"
    fi
}

validate_manifest_json()
{
    manifest=$1

    if have_command python3; then
        python3 -m json.tool "$manifest" >/dev/null ||
            die "Unicode data manifest is not valid JSON: $manifest"
        return
    fi

    if have_command python; then
        python -m json.tool "$manifest" >/dev/null ||
            die "Unicode data manifest is not valid JSON: $manifest"
        return
    fi

    die "python3 or python is required to validate Unicode data manifest JSON"
}

assert_existing_manifest()
{
    output_path=$1
    manifest=$(manifest_path "$output_path")

    if [ ! -f "$manifest" ]; then
        die "Unicode data manifest is missing: $manifest"
    fi

    validate_manifest_json "$manifest"
    expected_count=$(source_file_count)
    actual_count=$(awk '/"relative_path"[[:space:]]*:/ { count++ } END { print count + 0 }' "$manifest")
    require_manifest_string "$manifest" artifact_kind "$artifact_kind"
    require_manifest_string "$manifest" unicode_version "$unicode_version"
    require_manifest_string "$manifest" emoji_version "$emoji_version"

    if [ "$actual_count" != "$expected_count" ]; then
        die "manifest records $actual_count files, expected $expected_count"
    fi

    while IFS='|' read -r relative_path url; do
        [ -n "$relative_path" ] || continue

        path=$(artifact_path "$output_path" "$relative_path")
        if [ ! -f "$path" ]; then
            die "manifested Unicode data file is missing: $path"
        fi

        hash=$(sha256_file "$path") ||
            die "could not compute SHA-256 for $path"
        bytes=$(file_size_bytes "$path") ||
            die "could not determine byte size for $path"

        require_manifest_file_value "$manifest" "$relative_path" relative_path "$relative_path"
        require_manifest_file_value "$manifest" "$relative_path" url "$url"
        require_manifest_file_value "$manifest" "$relative_path" sha256 "$hash"
        require_manifest_file_value "$manifest" "$relative_path" bytes "$bytes"
    done <<EOF
$(source_files)
EOF
}

write_manifest()
{
    output_path=$1
    manifest=$(manifest_path "$output_path")
    manifest_name=$(basename "$manifest") || die "invalid manifest path: $manifest"
    count=0
    total=$(source_file_count)

    make_temp_file "$output_path" "$manifest_name"
    temp_path=$temp_path_to_clean

    {
        printf '%s\n' '{'
        printf '  "artifact_kind": "%s",\n' "$artifact_kind"
        printf '  "generated_by": "%s",\n' "$generated_by"
        printf '  "unicode_version": "%s",\n' "$unicode_version"
        printf '  "emoji_version": "%s",\n' "$emoji_version"
        printf '%s\n' '  "files": ['

        while IFS='|' read -r relative_path url; do
            [ -n "$relative_path" ] || continue
            count=$((count + 1))
            path=$(artifact_path "$output_path" "$relative_path")

            if [ ! -f "$path" ]; then
                die "manifest source file is missing: $path"
            fi

            hash=$(sha256_file "$path") ||
                die "could not compute SHA-256 for $path"
            bytes=$(file_size_bytes "$path") ||
                die "could not determine byte size for $path"

            printf '%s\n' '    {'
            printf '      "relative_path": "%s",\n' "$relative_path"
            printf '      "url": "%s",\n' "$url"
            printf '      "sha256": "%s",\n' "$hash"
            printf '      "bytes": %s\n' "$bytes"
            if [ "$count" -lt "$total" ]; then
                printf '%s\n' '    },'
            else
                printf '%s\n' '    }'
            fi
        done <<EOF
$(source_files)
EOF

        printf '%s\n' '  ]'
        printf '%s\n' '}'
    } > "$temp_path" || die "failed to write manifest"

    publish_file "$temp_path" "$manifest"
    printf '%s\n' "manifest: $manifest"
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

output_path=$(absolute_output_path "$output_directory")

ensure_directory "$output_path"
ensure_directory "$output_path/auxiliary"
ensure_directory "$output_path/emoji"

printf '%s\n' "Unicode data target: $output_path"
printf '%s\n' "Unicode version: $unicode_version; emoji version: $emoji_version"

if has_existing_unicode_artifact "$output_path" && [ "$force" != true ]; then
    if assert_existing_manifest "$output_path"; then
        printf '%s\n' "existing Unicode data manifest validated: $(manifest_path "$output_path")"
    else
        die "existing Unicode data is not backed by a valid manifest. Rerun with --force to redownload and write a fresh manifest."
    fi
else
    while IFS='|' read -r relative_path url; do
        [ -n "$relative_path" ] || continue
        download_file "$url" "$(artifact_path "$output_path" "$relative_path")"
    done <<EOF
$(source_files)
EOF

    write_manifest "$output_path"
fi

printf '%s\n' "Unicode data fetch complete: $output_path"
