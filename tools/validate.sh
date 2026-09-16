#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

run_static_checks() {
    local actionlint_bin
    local test_dir

    python3 tools/check_repo.py
    python3 tools/generate_starbridge_fonts.py --check
    python3 tools/generate_walkman_fonts.py --check
    python3 tools/check_walkman_assets.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_walkman_captions.py

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh)"
    fi
    "${actionlint_bin}" -color .github/workflows/*.yml

    test_dir="$(mktemp -d /tmp/ai-passport-host-tests.XXXXXX)"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c \
        -o "${test_dir}/test_ui_pixel_math"
    "${test_dir}/test_ui_pixel_math"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_demo_navigation.c main/demo_navigation.c \
        -o "${test_dir}/test_demo_navigation"
    "${test_dir}/test_demo_navigation"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_starbridge.c main/starbridge.c -o "${test_dir}/test_starbridge"
    "${test_dir}/test_starbridge"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_starbridge_sound.c main/starbridge_sound.c -o "${test_dir}/test_starbridge_sound"
    "${test_dir}/test_starbridge_sound"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_display_rounding"
    "${test_dir}/test_bsp_display_rounding"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_walkman.c main/walkman.c -o "${test_dir}/test_walkman"
    "${test_dir}/test_walkman"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_online_state.c main/online_state.c -o "${test_dir}/test_online_state"
    "${test_dir}/test_online_state"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_online_stream.c main/online_stream.c -o "${test_dir}/test_online_stream"
    "${test_dir}/test_online_stream"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_online_tts.c main/online_tts.c -o "${test_dir}/test_online_tts"
    "${test_dir}/test_online_tts"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_online_upload.c main/online_upload.c -o "${test_dir}/test_online_upload"
    "${test_dir}/test_online_upload"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_online_queue.c main/online_queue.c -o "${test_dir}/test_online_queue"
    "${test_dir}/test_online_queue"
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_deep_sleep_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_verify_firmware.py
    rm -rf "${test_dir}"
    echo "Host tests: PASS"
}

run_firmware_checks() (
    local validation_build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    validation_build_dir="$(mktemp -d /tmp/ai-passport-firmware.XXXXXX)"
    trap 'case "${validation_build_dir}" in /tmp/ai-passport-firmware.*) rm -rf -- "${validation_build_dir}" ;; esac' EXIT

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin"
    python3 tools/verify_firmware.py "${validation_build_dir}"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    echo "Firmware build: PASS"
)

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        usage
        exit 2
        ;;
esac
