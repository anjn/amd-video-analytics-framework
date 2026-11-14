"""Offline interval overlap for the failed minute windows, from recorded IDs."""
import csv,gzip,json,bisect,statistics,sys,hashlib
from pathlib import Path
root=Path(sys.argv[1]);analysis=json.loads((root/'frame-analysis.json').read_text())
origin=analysis['first_ns'];frames={};quotas=[];selected=[];rows=0
path=root/'diagnostics/events.csv.gz'
with gzip.open(path,'rt',newline='') as stream:
    reader=csv.reader(stream,escapechar='\\',doublequote=False);next(reader)
    for row in reader:
        rows+=1;ns=int(row[0]);elapsed=(ns-origin)/1e9
        if elapsed>1805:break
        source,event=row[1],row[4]
        if source=='controller' and event=='control_quota':quotas.append((ns-int(row[5]),ns))
        if event not in ['input','access_unit','decode_input','decode_output'] or not source.startswith('cam'):continue
        key=(source,int(row[2]),int(row[3]))
        frame=frames.setdefault(key,{})
        frame.setdefault(event,ns)
        if event=='decode_output':
            if all(k in frame for k in ['input','access_unit','decode_input']):
                minute=(ns-origin)//60_000_000_000
                if 25<=minute<=28:
                    selected.append(dict(source=source,minute=minute,sequence=key[2],generation=key[1],
                        input_ns=frame['input'],au_ns=frame['access_unit'],decoder_ns=frame['decode_input'],output_ns=ns))
            frames.pop(key,None)
        if rows%500000==0:frames={k:v for k,v in frames.items() if max(v.values())>ns-30_000_000_000}
quotas.sort();ends=[q[1] for q in quotas];groups={}
for frame in selected:
    wait=frame['decoder_ns']-frame['au_ns'];latency=frame['output_ns']-frame['input_ns']
    baseline=analysis['baseline_5_10'][frame['source']]['input_decode']['p99']
    if latency/1e6<=baseline+1000/30:continue
    start=bisect.bisect_right(ends,frame['au_ns']);overlap=0
    for a,b in quotas[start:]:
        if a>=frame['decoder_ns']:break
        overlap+=max(0,min(b,frame['decoder_ns'])-max(a,frame['au_ns']))
    item=groups.setdefault(frame['source'],{'high_frames':0,'quota_overlap_frames':0,'wait_ns':0,'quota_overlap_ns':0,'examples':[]})
    item['high_frames']+=1;item['quota_overlap_frames']+=int(overlap>0)
    item['wait_ns']+=wait;item['quota_overlap_ns']+=overlap
    if len(item['examples'])<4:item['examples'].append(dict(frame,input_decode_ms=latency/1e6,au_decoder_ms=wait/1e6,quota_overlap_ms=overlap/1e6))
for item in groups.values():
    item['frame_overlap_fraction']=item['quota_overlap_frames']/item['high_frames']
    item['wait_overlap_fraction']=item['quota_overlap_ns']/item['wait_ns'] if item['wait_ns'] else None
def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda:stream.read(2**20),b''):h.update(chunk)
    return h.hexdigest()
totals={k:sum(v[k] for v in groups.values()) for k in ['high_frames','quota_overlap_frames','wait_ns','quota_overlap_ns']}
result={'rows_read':rows,'window_minutes':[25,28],'groups':groups,'totals':totals,
        'trace_sha256':digest(path),'analysis_sha256':digest(root/'frame-analysis.json'),
        'script_sha256':digest(Path(__file__)),
        'high_frame_definition':'Input-to-decode exceeds this source\'s exact minutes 5–10 P99 by more than 1000/30 ms.',
        'note':'Exact interval overlap with control_quota during AU-to-decoder waiting. Overlap establishes a blocked dispatch interval, not attribution of every nanosecond to a disk operation.'}
(root/'control-overlap.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({k:{q:v for q,v in x.items() if q!='examples'} for k,x in groups.items()},indent=2))
