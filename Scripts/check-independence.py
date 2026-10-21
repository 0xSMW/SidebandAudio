#!/usr/bin/env python3
"""Fail on application/transport dependencies in SDK source and package metadata."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
forbidden = re.compile(r'\b(SidebandKit|URLSession|URLRequest|DownloadManager|PlaybackController|SwiftData|AppModel|waitingForDownload|receivedBytes)\b')
paths = [root / 'Package.swift', *sorted((root / 'Sources').rglob('*.swift')),
         *sorted((root / 'Sources').rglob('*.c')), *sorted((root / 'Sources').rglob('*.h'))]
violations = []
for path in paths:
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if forbidden.search(line):
            violations.append(f'{path.relative_to(root)}:{number}: {line.strip()}')
if violations:
    raise SystemExit('\n'.join(violations))
print('SDK independence check passed: no application or network transport dependencies.')
