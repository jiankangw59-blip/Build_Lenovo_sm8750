"""Run pure-Java contracts against production policy classes; never uses ADB."""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java-home', type=Path)
    args = parser.parse_args()
    candidates = [args.java_home]
    if os.environ.get('JAVA_HOME'):
        candidates.append(Path(os.environ['JAVA_HOME']))
    candidates += [Path('C:/Program Files/Microsoft/jdk-17.0.20.101-hotspot')]
    chosen = next((p for p in candidates if p and (p / 'bin/javac.exe').is_file()), None)
    javac = str(chosen / 'bin/javac.exe') if chosen else shutil.which('javac')
    java = str(chosen / 'bin/java.exe') if chosen else shutil.which('java')
    if not javac or not java:
        raise RuntimeError('JDK is missing; pass --java-home PATH')
    sources = [PROJECT / 'src/cn/xiaocaiye/scenesamplingopt' / name for name in ['SerialGate.java', 'RenderScope.java']]
    sources += sorted((HERE / 'src').rglob('*.java'))
    missing = [str(p) for p in sources if not p.is_file()]
    if missing:
        raise RuntimeError('Production test input missing: ' + ', '.join(missing))
    build = HERE / 'build' / dt.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    classes = build / 'classes'
    classes.mkdir(parents=True, exist_ok=False)
    captured = []
    digests = {}
    for source in sources:
        relative = source.relative_to(PROJECT)
        snapshot = build / 'source-inputs' / relative
        snapshot.parent.mkdir(parents=True, exist_ok=True)
        data = source.read_bytes()
        snapshot.write_bytes(data)
        captured.append(snapshot)
        digests[relative.as_posix()] = hashlib.sha256(data).hexdigest()
    compiler = subprocess.run([javac, '-encoding', 'UTF-8', '-source', '17', '-target', '17', '-Xlint:all', '-Werror', '-d', str(classes)] + [str(p) for p in captured], capture_output=True, text=True, encoding='utf-8', errors='replace')
    (build / 'compile.stdout.txt').write_text(compiler.stdout, encoding='utf-8')
    (build / 'compile.stderr.txt').write_text(compiler.stderr, encoding='utf-8')
    if compiler.returncode:
        raise RuntimeError('Test compilation failed:\n' + compiler.stdout + compiler.stderr)
    outcomes = []
    for name in ['GateContractTest', 'RenderScopeContractTest']:
        result = subprocess.run([java, '-cp', str(classes), 'cn.xiaocaiye.scenesamplingopt.' + name], capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=45)
        (build / (name + '.stdout.txt')).write_text(result.stdout, encoding='utf-8')
        (build / (name + '.stderr.txt')).write_text(result.stderr, encoding='utf-8')
        outcomes.append({'suite': name, 'exitCode': result.returncode, 'firstLine': result.stdout.splitlines()[0] if result.stdout else ''})
        print(result.stdout, end='')
        if result.stderr:
            print(result.stderr, file=sys.stderr, end='')
    report = {
        'scope': 'host isolated policy contracts; not Android Hook or power validation',
        'productionSources': {p.relative_to(PROJECT).as_posix(): digests[p.relative_to(PROJECT).as_posix()] for p in sources if 'tests' not in p.relative_to(PROJECT).parts},
        'testSources': {p.relative_to(PROJECT).as_posix(): digests[p.relative_to(PROJECT).as_posix()] for p in sources if 'tests' in p.relative_to(PROJECT).parts},
        'java': java,
        'build': str(build),
        'exitCode': 0 if all(item['exitCode'] == 0 for item in outcomes) else 1,
        'suites': outcomes,
        'deviceExecuted': False,
    }
    (build / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    (HERE / 'latest-host-report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('REPORT ' + str(build / 'report.json'))
    if report['exitCode']:
        raise SystemExit(report['exitCode'])


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
