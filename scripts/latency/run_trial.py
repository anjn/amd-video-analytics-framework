"""Run the unchanged interactive profile; diagnostics and resources are metadata only."""
import argparse,datetime as dt,gzip,hashlib,json,os,shutil,signal,subprocess as sp,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('name');p.add_argument('--seconds',type=int,default=3600)
p.add_argument('--diagnostics',action='store_true')
p.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[2])
p.add_argument('--runtime',type=Path,required=True)
p.add_argument('--output-root',type=Path,required=True)
p.add_argument('--ui-template',type=Path)
p.add_argument('--launcher',type=Path,help='Optional diagnostic launcher; excluded from final normal-mode acceptance')
args=p.parse_args()
if not args.name or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for c in args.name):p.error('Use a simple run name')
root=args.repo.resolve();base=args.output_root.resolve();base.mkdir(parents=True,exist_ok=True)
out=base/args.name;out.mkdir();runtime=args.runtime.resolve()
if args.ui_template:shutil.copy2(args.ui_template,runtime/'config.json.ui.json')
# Retain the exact runtime inputs and preparation hashes, not only their digest.
inputs=out/'inputs';inputs.mkdir()
input_hashes={}
for name in ['config.json','config.json.ui.json','preparation.json','demo.json','videos.txt','model-info.json','coco80.txt']:
    path=runtime/name
    if path.is_file():
        shutil.copy2(path,inputs/name);input_hashes[name]=hashlib.sha256(path.read_bytes()).hexdigest()
(out/'runtime-input-hashes.json').write_text(json.dumps(input_hashes,indent=2)+'\n')
launcher=args.launcher.resolve() if args.launcher else runtime/'run-demo.sh'
env=dict(os.environ,AMDVA_GPU_WAVE_MODE='sysfs',XDG_RUNTIME_DIR='/run/user/1000',WAYLAND_DISPLAY='wayland-0',QT_QPA_PLATFORM='wayland-egl',AMDVA_COPY_AUDIT='1')
env.pop('AMDVA_DIAGNOSTICS_DIR',None)
env['AMDVA_DIAGNOSTICS_DIR']=str(out/'diagnostics')
env['AMDVA_DIAGNOSTICS_TRACE']='1' if args.diagnostics else '0'
build_libraries={}
for candidate in (root/'build').rglob('*.so*'):
    if candidate.is_file():
        resolved=candidate.resolve()
        if str(resolved) not in build_libraries:build_libraries[str(resolved)]=hashlib.sha256(resolved.read_bytes()).hexdigest()
manifest={'launcher':str(launcher),'launcher_sha256':hashlib.sha256(launcher.read_bytes()).hexdigest(),'diagnostic_launcher':bool(args.launcher),'build_libraries_sha256':build_libraries,'rocm_root':env.get('AMDVA_ROCM_ROOT'),'migraphx_root':env.get('AMDVA_MIGRAPHX_ROOT'),'start_monotonic_ns':time.monotonic_ns(),'start_jst':dt.datetime.now().astimezone().isoformat(),'seconds':args.seconds,'diagnostics':args.diagnostics,
          'config_sha256':hashlib.sha256((runtime/'config.json').read_bytes()).hexdigest(),
          'ui_sha256':hashlib.sha256((runtime/'config.json.ui.json').read_bytes()).hexdigest() if (runtime/'config.json.ui.json').exists() else None,
          'binary_sha256':hashlib.sha256((root/'build/examples/nvr/amdva-nvr').read_bytes()).hexdigest(),
          'clock_ticks':os.sysconf('SC_CLK_TCK'),'exit_reason':'duration'}
sp.run(['git','diff','--binary'],cwd=root,stdout=(out/'source.patch').open('w'),check=True)
import tarfile
files=sp.check_output(['git','ls-files','--cached','--others','--exclude-standard','-z'],cwd=root).decode().split('\0')
with tarfile.open(out/'source.tgz','w:gz') as archive:
    for file in files:
        if file and (root/file).is_file():archive.add(root/file,arcname=file)
manifest['source_archive_sha256']=hashlib.sha256((out/'source.tgz').read_bytes()).hexdigest()
(out/'manifest.json').write_text(json.dumps(manifest,indent=2))
def read(path):
    try:return Path(path).read_text()
    except (OSError,PermissionError):return ''
def sample(pid):
    row={'elapsed_s':time.monotonic()-start,'wall_jst':dt.datetime.now().astimezone().isoformat(),
         'free_bytes':shutil.disk_usage(root).free,'pid':pid}
    if pid:
        proc=Path('/proc')/str(pid)
        row.update(status=read(proc/'status'),io=read(proc/'io'),stat=read(proc/'stat'))
        row['fd_count']=len(list((proc/'fd').glob('*')))
        row['drm']=[read(q) for q in (proc/'fdinfo').glob('*') if 'drm-driver' in read(q)]
        row['threads']={q.name:{'name':read(q/'comm').strip(),'wchan':read(q/'wchan').strip(),'stat':read(q/'stat')} for q in (proc/'task').glob('*')}
    return row
log=(out/'launcher.log').open('w');resource=(out/'resources.jsonl').open('w')
process=sp.Popen([str(launcher)],cwd=root,env=env,stdout=log,stderr=sp.STDOUT,start_new_session=True)
start=time.monotonic();deadline=start+args.seconds;pid=None;maps_recorded=False;previous_status=None;previous_status_ns=None;baseline={};first_symptom=None;ai_stale={};last_status_seen=start
try:
    while time.monotonic()<deadline:
        if process.poll() is not None:manifest['exit_reason']='launcher_exit';break
        if pid is None:
            pending=[process.pid];seen=set()
            while pending and len(seen)<64:
                parent=pending.pop();seen.add(parent)
                for child in read(f'/proc/{parent}/task/{parent}/children').split():
                    if read(f'/proc/{child}/comm').strip()=='amdva-nvr':pid=int(child);break
                    if int(child) not in seen:pending.append(int(child))
                if pid is not None:break
        if pid and not maps_recorded and time.monotonic()-start>=120:
            mappings=sorted({line.split(maxsplit=5)[5] for line in read(f'/proc/{pid}/maps').splitlines()
                             if len(line.split(maxsplit=5))==6 and line.split(maxsplit=5)[5].startswith('/')})
            selected={}
            for name in mappings:
                path=Path(name)
                if root in path.parents and '.so' in path.name and path.is_file():
                    selected[name]=hashlib.sha256(path.read_bytes()).hexdigest()
            (out/'loaded-libraries.json').write_text(json.dumps({'elapsed_s':time.monotonic()-start,'mappings':mappings,'framework_library_sha256':selected},indent=2))
            maps_recorded=True
        row=sample(pid);resource.write(json.dumps(row)+'\n');resource.flush()
        status_path=out/'diagnostics/status.jsonl'
        recent_status=None
        if status_path.exists():
            with status_path.open('rb') as source:
                size=source.seek(0,2);source.seek(max(0,size-600000))
                for line in reversed(source.read().splitlines()):
                    try:recent_status=json.loads(line);break
                    except ValueError:pass
        symptoms=[]
        if recent_status and recent_status['ns']!=previous_status_ns:
            last_status_seen=time.monotonic()
            ns=recent_status['ns'];elapsed=row['elapsed_s']
            for timing in recent_status['telemetry']:
                latency=timing['continuous']['latency']['input_decode'];key=timing['source']
                stages=timing['continuous']['stages']
                if elapsed>=120 and stages['input']['active'] and stages['input']['fps']>0:
                    for stage in ['decode_output','draw','swap']:
                        if stages[stage]['supported'] and stages[stage]['active'] and stages[stage]['stalled']:
                            symptoms.append(key+': '+stage+' stalled')
                if latency['samples'] and 300<=elapsed<=600:
                    baseline.setdefault(key,[]).append(latency['p99_ms'])
                if latency['samples'] and latency['mean_ms']>=150:
                    symptoms.append(key+': input_decode_mean >=150ms')
                elif baseline.get(key) and elapsed>600 and latency['samples'] and latency['p99_ms']>max(baseline[key])+(100 if key=='resource-monitor' else 1000/30):
                    symptoms.append(key+': input_decode_p99 increased')
            if previous_status:
                old={c['id']:c for c in previous_status['channels']}
                for channel in recent_status['channels']:
                    prev=old.get(channel['id']);key=channel['id']
                    if prev and channel['decoded']>prev['decoded'] and channel['ai']==prev['ai'] and channel['ai']>0:
                        ai_stale[key]=ai_stale.get(key,0)+1
                        if ai_stale[key]>=2:symptoms.append(key+': AI progress stopped')
                    else:ai_stale[key]=0
            previous_status=recent_status;previous_status_ns=ns
        if pid and time.monotonic()-last_status_seen>30:symptoms.append('status capture stopped')
        if symptoms and first_symptom is None and args.seconds>=3600:
            first_symptom=row['elapsed_s'];manifest['first_symptom']={'elapsed_s':first_symptom,'signals':symptoms}
            deadline=max(deadline,time.monotonic()+600)
            (out/'symptom.json').write_text(json.dumps(manifest['first_symptom'],indent=2))
        (out/'progress.json').write_text(json.dumps({'elapsed_s':round(row['elapsed_s']), 'pid':pid,'free_gib':round(row['free_bytes']/2**30,2),'first_symptom':first_symptom}))
        if row['free_bytes']<2*2**30:manifest['exit_reason']='low_disk';break
        time.sleep(5)
finally:
    if process.poll() is None:
        if pid is not None:
            try:os.kill(pid,signal.SIGTERM)
            except ProcessLookupError:process.send_signal(signal.SIGTERM)
        else:process.send_signal(signal.SIGTERM)
        try:process.wait(timeout=90)
        except sp.TimeoutExpired:
            manifest['exit_reason']='shutdown_timeout';os.killpg(process.pid,signal.SIGKILL);process.wait()
    manifest.update(returncode=process.returncode,elapsed_s=time.monotonic()-start,end_jst=dt.datetime.now().astimezone().isoformat())
    resource.close();log.close()
    if (runtime/'config.json.ui.json').exists():shutil.copy2(runtime/'config.json.ui.json',out/'final-ui.json')
    if (runtime/'logs').exists():shutil.copytree(runtime/'logs',out/'logs')
    sp.run(['journalctl','-k','--since',manifest['start_jst'],'--no-pager'],stdout=(out/'kernel.log').open('w'),stderr=sp.STDOUT)
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
    events=out/'diagnostics/events.csv'
    if events.exists():
        with events.open('rb') as src,gzip.open(str(events)+'.gz','wb',compresslevel=1) as dst:shutil.copyfileobj(src,dst)
        events.unlink()
print(json.dumps(manifest),flush=True)

if manifest["exit_reason"]!="duration":raise SystemExit(1)
