#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$root/.build"
build_number=$(python3 - "$root/.build/demo-build-number" <<'PY'
from pathlib import Path
import fcntl, sys
with Path(sys.argv[1]).open('a+') as f:
    fcntl.flock(f, fcntl.LOCK_EX)
    f.seek(0)
    number = int(f.read().strip() or '0') + 1
    f.seek(0); f.truncate(); f.write(str(number)); f.flush()
    print(number)
PY
)
destination=${1:-generic/platform=iOS Simulator}
printf 'Building Sideband Audio Demo 0.1.0 (%s)\n' "$build_number"
xcodebuild -project "$root/Examples/SidebandAudioDemo/SidebandAudioDemo.xcodeproj" \
    -scheme SidebandAudioDemo -configuration Release -destination "$destination" \
    -derivedDataPath "$root/.build/DemoDerivedData" \
    CURRENT_PROJECT_VERSION="$build_number" CODE_SIGNING_ALLOWED=NO build
