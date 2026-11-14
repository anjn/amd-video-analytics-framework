"""Require active, complete copy and exact VPE-fence audits for final trials."""
import argparse,json
from pathlib import Path

def validate_audits(directory,minimum_seconds=3600):
    directory=Path(directory);failures=[]
    def read(name):
        try:return json.loads((directory/name).read_text())
        except (OSError,ValueError):
            failures.append({'reason':'missing or invalid evidence','file':name});return {}
    manifest=read('manifest.json')
    if manifest.get('diagnostic_launcher') or manifest.get('exit_reason')!='duration' or manifest.get('returncode')!=0:
        failures.append({'reason':'requires normal launcher, full duration and clean exit'})
    copy=read('diagnostics/copy-audit-summary.json')
    coverage=(copy.get('active_end_ns',0)-copy.get('active_start_ns',0))/1e9
    if not copy.get('requested') or not copy.get('active_start_ns') or coverage<minimum_seconds:
        failures.append({'reason':'copy audit missing or coverage too short','seconds':coverage})
    if copy.get('violations')!=0:failures.append({'reason':'copy audit violations or missing result'})
    vpe=read('diagnostics/vpe-audit-summary.json')
    if vpe.get('method')!='binary-syncobj-fence' or not vpe.get('completed') or vpe.get('scopes')!=vpe.get('completed') or vpe.get('submissions',0)<vpe.get('completed',0):
        failures.append({'reason':'VPE operations not fully proved by exact completion fences'})
    for key in ['failed','aborted','rejected_submissions','foreign_operations','active_scope']:
        if vpe.get(key)!=0:failures.append({'reason':'VPE audit incomplete or failed','counter':key,'value':vpe.get(key)})
    active_rows=0;previous=None
    path=directory/'diagnostics/status.jsonl'
    if path.exists():
        with path.open() as stream:
            for line in stream:
                row=json.loads(line);c=row.get('copy_audit',{});v=row.get('vpe_audit',{})
                if c.get('active'):
                    active_rows+=1
                    if c.get('violations')!=0 or not v.get('available') or any(v.get(key)!=0 for key in ['failed','aborted','rejected','foreign']):
                        failures.append({'reason':'live audit failure','ns':row.get('ns')});break
                    if previous and (c.get('start_ns')!=previous['start_ns'] or c.get('copies',0)<previous.get('copies',0)):
                        failures.append({'reason':'copy audit reset during trial'});break
                    previous=c
    if not active_rows:failures.append({'reason':'no live active audit evidence'})
    result={'audits_passed':not failures,'copy_active_seconds':coverage,'active_snapshots':active_rows,'vpe':vpe,'failures':failures}
    (directory/'audit-validation.json').write_text(json.dumps(result,indent=2)+'\n');return result

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('directory',type=Path);args=p.parse_args()
    r=validate_audits(args.directory);print(json.dumps(r,indent=2))
