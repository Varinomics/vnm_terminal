#!/usr/bin/env bash
set -euo pipefail

verifier="$1"
app_bundle="$2"
architecture="$3"
fixture_dir="$(mktemp -d)"
trap 'rm -rf "${fixture_dir}"' EXIT
mkdir "${fixture_dir}/bin"

# Keep real Mach-O and architecture inspection; vary only nm's symbol records.
cat > "${fixture_dir}/bin/nm" <<'EOF'
#!/usr/bin/env bash
cat "${NM_FIXTURE}"
EOF
chmod +x "${fixture_dir}/bin/nm"

check_symbols()
{
    local name="$1" expected_status="$2" symbols="$3" actual_status=0
    printf '%s\n' "${symbols}" > "${fixture_dir}/symbols.txt"
    NM_FIXTURE="${fixture_dir}/symbols.txt" \
        PATH="${fixture_dir}/bin:${PATH}" \
        bash "${verifier}" "${app_bundle}" "${architecture}" \
        > "${fixture_dir}/result.txt" 2>&1 || actual_status=$?
    if [[ "${actual_status}" != "${expected_status}" ]]; then
        echo "${name}: expected status ${expected_status}, found ${actual_status}" >&2
        cat "${fixture_dir}/result.txt" >&2
        exit 1
    fi
    if [[ "${expected_status}" == 1 ]]; then
        grep -q 'Debug information is present' "${fixture_dir}/result.txt"
    fi
}

placeholder='0000000005614542 - 00 0000 OPT radr://5614542'
check_symbols 'Runtime symbols' 0 '0000000100001000 T _main'
check_symbols 'Apple strip placeholder' 0 "${placeholder}"
check_symbols 'Debug record beside placeholder' 1 "${placeholder}
0000000000000000 - 00 0000 SO /Users/build/source/terminal.cpp"
check_symbols 'Other N_OPT record' 1 '0000000000000000 - 00 0000 OPT gcc_compiled'
check_symbols 'Placeholder name with different value' 1 '0000000000000000 - 00 0000 OPT radr://5614542'

echo 'macOS bundle debug-symbol classification passed.'
