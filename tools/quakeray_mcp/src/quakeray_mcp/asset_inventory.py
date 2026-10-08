import hashlib
from pathlib import Path
import re

from .errors import EvidenceError


def inventory(runtime):
    runtime = Path(runtime).resolve()
    entries = []
    for directory in ('id1', 'ad'):
        for path in sorted((runtime / directory).rglob('*')):
            if not path.is_file():
                continue
            name = path.name.lower()
            direct_game_file = len(path.relative_to(runtime).parts) == 2
            generated = name in {'config.cfg', 'benchmark.log', 'qperfdump.log', 'crash.log'} or re.fullmatch(
                r'(?:stats-\d{8}-\d{6}\.dump|benchmark-frames-[A-Za-z0-9_-]+\.csv|screenshot\d+\.png|perf-[A-Za-z0-9_.-]+\.log|(?:qs_|qr_m_)[a-f0-9]+\.cfg)', name)
            if direct_game_file and generated:
                continue
            resolved = path.resolve()
            if not resolved.is_relative_to(runtime):
                raise EvidenceError('PERMISSION_DENIED', 'Asset inventory contains a redirected path')
            digest = hashlib.sha256()
            with path.open('rb') as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(chunk)
            entries.append({'File': path.relative_to(runtime).as_posix(), 'Sha256': digest.hexdigest().upper()})
            if len(entries) > 10000:
                raise EvidenceError('FILE_TOO_LARGE', 'Asset inventory exceeds its entry bound')
    return entries


if __name__ == '__main__':
    import json
    import sys
    print(json.dumps(inventory(sys.argv[1])))
