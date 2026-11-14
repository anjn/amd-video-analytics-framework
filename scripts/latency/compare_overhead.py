import argparse,json,statistics
from pathlib import Path
def read_rows(path):return [json.loads(s) for s in path.read_text().splitlines() if s]
def summary(path):
    path=Path(path);rows=read_rows(path/'diagnostics/status.jsonl');t0=rows[0]['ns']
    rows=[r for r in rows if (r['ns']-t0)/1e9>=60]
    a,b=rows[0],rows[-1];seconds=(b['ns']-a['ns'])/1e9
    first={c['id']:c for c in a['channels']};last={c['id']:c for c in b['channels']}
    channels={k:{'ai_hz':(c['ai']-first[k]['ai'])/seconds,
                 'decode_hz':(c['decoded']-first[k]['decoded'])/seconds,
                 'display_fps':statistics.mean(next(v['display_fps'] for v in r['channels'] if v['id']==k) for r in rows)}
              for k,c in last.items() if k.startswith('cam') and c['decoded']>0}
    resources=[r for r in read_rows(path/'resources.jsonl') if r['pid'] and r['elapsed_s']>=80]
    r0,r1=resources[0],resources[-1]
    cpu=lambda r:sum(int(v) for v in r['stat'].split()[13:15])/100
    ai={}
    for r in rows:
        for m in r.get('ai',[]):
            if not m['camera'] and m['completed'] and 'inference' in m['stage'].lower():ai.setdefault(m['stage'],[]).append(m['p95_ms'])
    errors=[(c['id'],c['error']) for r in rows for c in r['channels'] if c['error']]
    return {'channels':channels,'ai_inference_p95_ms':{k:statistics.mean(v) for k,v in ai.items()},
            'cpu_cores':(cpu(r1)-cpu(r0))/(r1['elapsed_s']-r0['elapsed_s']),
            'errors':errors,'seconds':seconds}
p=argparse.ArgumentParser();p.add_argument('off');p.add_argument('on');a=p.parse_args()
off,on=summary(a.off),summary(a.on)
changes={'cpu_cores_added':on['cpu_cores']-off['cpu_cores']}
for key in ['ai_hz','decode_hz','display_fps']:
    x=statistics.mean(v[key] for v in off['channels'].values());y=statistics.mean(v[key] for v in on['channels'].values())
    changes[key+'_pct']=100*(y/x-1)
changes['ai_inference_p95_pct']={k:100*(v/off['ai_inference_p95_ms'][k]-1) for k,v in on['ai_inference_p95_ms'].items() if k in off['ai_inference_p95_ms']}
result={'off':off,'on':on,'change':changes,
        'note':'Sequential short trials; affected by temporal variance. CPU uses verified Linux USER_HZ=100.'}
Path(a.on).joinpath('overhead-comparison.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(changes,indent=2))
