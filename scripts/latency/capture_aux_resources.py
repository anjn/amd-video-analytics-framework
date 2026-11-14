import argparse,json,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('trial',type=Path);p.add_argument('--repo',type=Path,required=True);a=p.parse_args()
trial=a.trial.resolve();root=a.repo.resolve();progress=json.loads((trial/'progress.json').read_text());nvr=progress['pid']
def read(pid,name):
 try:return (Path('/proc')/str(pid)/name).read_text()
 except OSError:return ''
def identity(pid):
 try:
  exe=(Path('/proc')/str(pid)/'exe').resolve(strict=True)
  stat=read(pid,'stat');return str(exe),stat.split(') ',1)[1].split()[19]
 except (OSError,IndexError):return None
parent=int(next(v.split(':',1)[1] for v in read(nvr,'status').splitlines() if v.startswith('PPid:')))
expected={str(root/'build/examples/rtsp-files/amdva-rtsp-files'):'rtsp',str(root/'build/examples/resource-dashboard/amdva-dashboard'):'dashboard'}
tracked={}
for child in read(parent,f'task/{parent}/children').split():
 value=identity(int(child))
 if value and value[0] in expected:tracked[int(child)]={'role':expected[value[0]],'identity':value}
assert len(tracked)==2,tracked
(trial/'aux-monitor-metadata.json').write_text(json.dumps({'schema':1,'started_ns':time.monotonic_ns(),'main_pid':nvr,'parent_pid':parent,'processes':tracked,'period_seconds':10,'note':'Passive process metadata only; no GPU profiling or application changes.'},indent=2)+'\n')
gone=None
with (trial/'aux-resources.jsonl').open('x') as out:
 while True:
  now=time.monotonic_ns();items=[]
  for pid,value in tracked.items():
   alive=identity(pid)==tuple(value['identity'])
   items.append(dict(pid=pid,role=value['role'],alive=alive,status=read(pid,'status') if alive else '',stat=read(pid,'stat') if alive else '',io=read(pid,'io') if alive else ''))
  out.write(json.dumps({'ns':now,'processes':items})+'\n');out.flush()
  if not any(v['alive'] for v in items):break
  if not read(nvr,'stat'):
   if gone is None:gone=now
   if now-gone>120_000_000_000:
    (trial/'aux-process-leak.json').write_text(json.dumps(items,indent=2)+'\n');raise SystemExit('Owned helper remains after NVR exit')
  time.sleep(10)
print('Owned helper processes exited; passive monitoring complete.',flush=True)
