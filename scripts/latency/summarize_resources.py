"""Summarize passive resource samples offline; never declares absence of leaks."""
import argparse,json,statistics
from pathlib import Path

def fields(text):
    return dict(line.split(':',1) for line in text.splitlines() if ':' in line)

def integer(value):
    return int(value.split()[0])

def cpu_cores(previous,current,ticks):
    elapsed=current[0]-previous[0];delta=current[1]-previous[1]
    return delta/ticks/elapsed if elapsed>0 and delta>=0 else None

def drm_totals(records):
    # Several descriptors can refer to the same DRM client. Count each once.
    clients={}
    for record in records:
        values=fields(record)
        if 'drm-client-id' not in values:continue
        key=(values.get('drm-pdev','').strip(),values['drm-client-id'].strip())
        clients[key]=values
    return {kind:sum(integer(v.get('drm-memory-'+kind,'0')) for v in clients.values())
            for kind in ['vram','gtt','cpu']}

def describe(points):
    if not points:return None
    xs,ys=zip(*points);xmean=statistics.mean(xs);ymean=statistics.mean(ys)
    denominator=sum((x-xmean)**2 for x in xs)
    return {'samples':len(points),'first':ys[0],'last':ys[-1],
            'minimum':min(ys),'maximum':max(ys),'median':statistics.median(ys),
            'slope_per_minute':60*sum((x-xmean)*(y-ymean) for x,y in points)/denominator if denominator else None}

def summarize(directory):
    directory=Path(directory);samples=[];previous_cpu=None
    ticks=json.loads((directory/'manifest.json').read_text())['clock_ticks']
    with (directory/'resources.jsonl').open() as stream:
        for line in stream:
            row=json.loads(line)
            if not row.get('stat'):continue
            status=fields(row.get('status',''));io=fields(row.get('io',''))
            item={'elapsed_s':row['elapsed_s'],'rss_kib':integer(status.get('VmRSS','0')),
                  'rss_anon_kib':integer(status.get('RssAnon','0')),
                  'rss_file_kib':integer(status.get('RssFile','0')),
                  'rss_shmem_kib':integer(status.get('RssShmem','0')),
                  'virtual_kib':integer(status.get('VmSize','0')),
                  'fd_count':row.get('fd_count',0),'threads':integer(status.get('Threads','0')),
                  'free_bytes':row['free_bytes'],**{'drm_'+k+'_kib':v for k,v in drm_totals(row.get('drm',[])).items()},
                  **{k:integer(io.get(k,'0')) for k in ['read_bytes','write_bytes','cancelled_write_bytes']}}
            stat=row['stat'].rsplit(') ',1)[1].split()
            current_cpu=(row['elapsed_s'],int(stat[11])+int(stat[12]))
            item['cpu_cores']=cpu_cores(previous_cpu,current_cpu,ticks) if previous_cpu else None
            previous_cpu=current_cpu;samples.append(item)
    windows={}
    windows_to_review=[('baseline_5_10',300,600),('late_20_60',1200,3600),('final_10',max(0,samples[-1]['elapsed_s']-600) if samples else 0,float('inf'))]
    windows_to_review.extend((f'minutes_{start}_{start+10}',start*60,(start+10)*60) for start in range(10,60,10))
    for name,start,end in windows_to_review:
        selected=[r for r in samples if start<=r['elapsed_s']<end]
        windows[name]={key:describe([(r['elapsed_s'],r[key]) for r in selected if r[key] is not None]) for key in samples[0] if key!='elapsed_s'} if samples else {}
    auxpath=directory/'aux-resources.jsonl';aux={}
    if auxpath.exists():
        rows=[json.loads(line) for line in auxpath.open()]
        for row in rows:
            for process in row['processes']:
                value=aux.setdefault(process['role'],{'rss':[],'last_alive':None})
                value['last_alive']=process['alive']
                if process['alive']:
                    value['rss'].append((row['ns']/1e9,integer(fields(process['status']).get('VmRSS','0'))))
        aux={k:{'rss_kib':describe(v['rss']),'last_alive':v['last_alive']} for k,v in aux.items()}
    return {'samples':len(samples),'windows':windows,'helpers':aux,
            'helper_leak_report_exists':(directory/'aux-process-leak.json').exists(),
            'note':'Observed slopes and ranges, not proof of no leak. DRM allocations deduplicate shared client descriptors. KiB units follow kernel fdinfo. Review kernel logs and frame/AI/audit results separately.'}

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('directory',type=Path);args=parser.parse_args()
    result=summarize(args.directory)
    (args.directory/'resource-analysis.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
