"""Compile production Mini render hooks with pinned ABI fixtures; never uses ADB."""
from pathlib import Path
import datetime as dt
import hashlib
import json
import re
import subprocess
import sys
sys.stdout.reconfigure(encoding='utf-8')
HERE=Path(__file__).resolve().parent
PROJECT=HERE.parent
JAVA=Path(r'C:\Program Files\Microsoft\jdk-17.0.20.101-hotspot\bin')
sources=[PROJECT/'src/cn/xiaocaiye/scenesamplingopt/MiniRenderDeduplicator.java']
sources+=sorted((HERE/'mini-fixtures').rglob('*.java'))
shared=HERE/'thread-guard-fixtures'
sources += [p for p in sorted(shared.rglob('*.java')) if p.relative_to(shared).parts[0] in ('io','org')]
sources += [shared/'android/os/Handler.java',shared/'android/os/Looper.java']
out=HERE/'build'/('mini-'+dt.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
classes=out/'classes';classes.mkdir(parents=True)
inputs=[];digests={}
for p in sources:
    relative=p.relative_to(PROJECT);raw=p.read_bytes();q=out/'source-inputs'/relative;q.parent.mkdir(parents=True,exist_ok=True);q.write_bytes(raw)
    inputs.append(q);digests[relative.as_posix()]=hashlib.sha256(raw).hexdigest()
compiler=subprocess.run([str(JAVA/'javac.exe'),'-J-Duser.language=en','-J-Duser.country=US','-encoding','UTF-8','-source','17','-target','17','-Xlint:all','-d',str(classes)]+list(map(str,inputs)),capture_output=True,text=True,encoding='utf-8',errors='replace')
(out/'compile.stdout.txt').write_text(compiler.stdout,encoding='utf-8');(out/'compile.stderr.txt').write_text(compiler.stderr,encoding='utf-8')
print(compiler.stdout+compiler.stderr,end='')
if compiler.returncode:raise SystemExit(compiler.returncode)
result=subprocess.run([str(JAVA/'java.exe'),'-cp',str(classes),'cn.xiaocaiye.scenesamplingopt.MiniRenderContractTest'],capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=30)
(out/'test.stdout.txt').write_text(result.stdout,encoding='utf-8');(out/'test.stderr.txt').write_text(result.stderr,encoding='utf-8')
print(result.stdout+result.stderr,end='')
counts=re.search(r'MINI_RENDER_CONTRACTS cases=(\d+) assertions=(\d+)',result.stdout)
report=dict(scope='host production Mini Hook-contract fixtures; not Android rendering or power validation',productionSourceSha256=digests['src/cn/xiaocaiye/scenesamplingopt/MiniRenderDeduplicator.java'],sourceSha256=digests,exitCode=result.returncode,build=str(out),deviceExecuted=False,contractCount=int(counts.group(1))if counts else None,assertionCount=int(counts.group(2))if counts else None)
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');(HERE/'latest-mini-host-report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print('REPORT',out/'report.json');raise SystemExit(result.returncode)
