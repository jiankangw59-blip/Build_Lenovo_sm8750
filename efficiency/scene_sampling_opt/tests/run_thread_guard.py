"""Compile production ThreadWindowGuard with pinned ABI fixtures; no ADB or Android runtime claims."""
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
sources=[PROJECT/'src/cn/xiaocaiye/scenesamplingopt/ThreadWindowGuard.java']
sources+=sorted((HERE/'thread-guard-fixtures').rglob('*.java'))
out=HERE/'build'/('thread-guard-'+dt.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
classes=out/'classes';classes.mkdir(parents=True)
captured=[]
digests={}
for p in sources:
    relative=p.relative_to(PROJECT)
    raw=p.read_bytes()
    q=out/'source-inputs'/relative;q.parent.mkdir(parents=True,exist_ok=True);q.write_bytes(raw)
    captured.append(q)
    digests[relative.as_posix()]=hashlib.sha256(raw).hexdigest()
compiler=subprocess.run([str(JAVA/'javac.exe'),'-J-Duser.language=en','-J-Duser.country=US','-encoding','UTF-8','-source','17','-target','17','-Xlint:all','-d',str(classes)]+list(map(str,captured)),capture_output=True,text=True,encoding='utf-8',errors='replace')
(out/'compile.stdout.txt').write_text(compiler.stdout,encoding='utf-8')
(out/'compile.stderr.txt').write_text(compiler.stderr,encoding='utf-8')
print(compiler.stdout+compiler.stderr,end='')
if compiler.returncode: raise SystemExit(compiler.returncode)
test=subprocess.run([str(JAVA/'java.exe'),'-cp',str(classes),'cn.xiaocaiye.scenesamplingopt.ThreadWindowGuardContractTest'],capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=30)
(out/'test.stdout.txt').write_text(test.stdout,encoding='utf-8')
(out/'test.stderr.txt').write_text(test.stderr,encoding='utf-8')
print(test.stdout+test.stderr,end='')
counts=re.search(r'THREAD_GUARD_CONTRACTS cases=(\d+) assertions=(\d+)',test.stdout)
report=dict(scope='host production Hook-contract fixtures; not Android execution or power validation',productionSourceSha256=digests['src/cn/xiaocaiye/scenesamplingopt/ThreadWindowGuard.java'],sourceSha256=digests,exitCode=test.returncode,build=str(out),deviceExecuted=False,contractCount=int(counts.group(1)) if counts else None,assertionCount=int(counts.group(2)) if counts else None)
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
(HERE/'latest-thread-guard-host-report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print('REPORT',out/'report.json')
raise SystemExit(test.returncode)
