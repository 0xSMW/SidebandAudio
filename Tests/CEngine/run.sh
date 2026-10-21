#!/bin/sh
# Standalone C gates only: no Swift or app build, no cleanup of existing artifacts.
set -eu
package=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build="$package/.build/gates"
portable_flags=""
if [ "${SB_ENGINE_PORTABLE:-0}" = 1 ]; then
    portable_flags="-DSB_ENGINE_PORTABLE"
    build="$package/.build/gates-portable"
fi
src="$package/Sources/SBEngine"
mkdir -p "$build"
compiler=${CC:-clang}
platform_libs=""
if [ "$(uname -s)" = Darwin ]; then platform_libs="-framework Accelerate"; fi
for gate in stretch dsp engine eq_response fft correlation mono timeline bridge_eof pause; do
    printf '\nCompiling %s gate with %s\n' "$gate" "$compiler"
    stretcher_source="$src/sb_stretcher.c"
    dsp_source="$src/sb_dsp.c"
    if [ "$gate" = correlation ]; then stretcher_source=""; fi
    if [ "$gate" = mono ]; then dsp_source=""; fi
    "$compiler" -std=c11 -O1 -g -Wall -Wextra -Werror -pedantic $portable_flags \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        -I "$src" -I "$src/include" "$package/Tests/CEngine/${gate}_test.c" \
        $stretcher_source $dsp_source "$src/sb_engine.c" "$src/sb_timeline.c" \
        -lm $platform_libs -o "$build/${gate}_test"
    # LeakSanitizer is unavailable in Apple's ASan runtime. Both ASan and UBSan
    # remain enabled; callers on supported hosts may override ASAN_OPTIONS.
    if ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" \
       UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}" \
       "$build/${gate}_test" > "$build/${gate}.log" 2>&1; then
        cat "$build/${gate}.log"
    else
        cat "$build/${gate}.log" >&2
        exit 1
    fi
done
printf '\nAll ten SBEngine ASan+UBSan gates passed. Logs: %s\n' "$build"
