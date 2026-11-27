"""Compile the production refresh interceptors with pinned original-behavior fixtures.

Portable CI runner. Does not build an APK, use ADB, or claim ART/device success.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    java_home = args.java_home or (Path(os.environ['JAVA_HOME']) if os.environ.get('JAVA_HOME') else None)
    suffix = '.exe' if os.name == 'nt' else ''
    javac = str(java_home / 'bin' / ('javac' + suffix)) if java_home else shutil.which('javac')
    java = str(java_home / 'bin' / ('java' + suffix)) if java_home else shutil.which('java')
    if not javac or not java:
        raise RuntimeError('JDK 17+ missing; run in CI or pass --java-home')
    sources = [PROJECT / 'src/cn/xiaocaiye/scenesamplingopt/RefreshRatePersistence.java']
    sources += sorted((HERE / 'refresh-fixtures').rglob('*.java'))
    if any(not source.is_file() for source in sources):
        raise RuntimeError('Missing production source or contract fixture')
    out = args.output or HERE / 'build' / ('refresh-' + dt.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    classes = out / 'classes'
    classes.mkdir(parents=True, exist_ok=False)
    digests = {p.relative_to(PROJECT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    compile_result = subprocess.run([javac, '-J-Xms16m', '-J-Xmx128m', '-encoding', 'UTF-8',
                                    '--release', '17', '-Xlint:all', '-Werror', '-d', str(classes)]
                                   + [str(p) for p in sources], capture_output=True, text=True,
                                   encoding='utf-8', errors='replace', timeout=90)
    (out / 'compile.stdout.txt').write_text(compile_result.stdout, encoding='utf-8')
    (out / 'compile.stderr.txt').write_text(compile_result.stderr, encoding='utf-8')
    if compile_result.returncode:
        raise RuntimeError('Contract compilation failed:\n' + compile_result.stdout + compile_result.stderr)
    test = subprocess.run([java, '-Xms8m', '-Xmx64m', '-cp', str(classes),
                           'cn.xiaocaiye.scenesamplingopt.RefreshRatePersistenceContractTest'],
                          capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=45)
    (out / 'test.stdout.txt').write_text(test.stdout, encoding='utf-8')
    (out / 'test.stderr.txt').write_text(test.stderr, encoding='utf-8')
    matched = re.search(r'REFRESH_PERSISTENCE_CONTRACTS cases=(\d+) assertions=(\d+)', test.stdout)
    report = {'scope': 'production interceptor contracts with pinned DEX behavior fixtures; not ART/device',
              'deviceExecuted': False, 'sourceSha256': digests, 'exitCode': test.returncode,
              'contracts': int(matched.group(1)) if matched else None,
              'assertions': int(matched.group(2)) if matched else None,
              'javacMaxHeapMiB': 128, 'testMaxHeapMiB': 64}
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(test.stdout, end='')
    if test.stderr:
        print(test.stderr, file=sys.stderr, end='')
    print('REPORT', out / 'report.json')
    if test.returncode or not matched:
        raise SystemExit(test.returncode or 1)


if __name__ == '__main__':
    main()
