"""Offline figure from exact minute distributions; matplotlib is analysis-only."""
import argparse,json,hashlib,sys
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

p=argparse.ArgumentParser();p.add_argument('analyses',nargs='+',type=Path)
p.add_argument('--output',required=True,type=Path);a=p.parse_args()
fig,axes=plt.subplots(3,1,figsize=(11,10),sharex=True,layout='constrained')
colors=['#b44b3f','#287db1','#26855d','#8056a4']
provenance=[]
for index,path in enumerate(a.analyses):
    result=json.loads(path.read_text(encoding='utf-8'));name=result.get('trial',path.parent.name)
    acceptance=path.parent/'acceptance.json'
    if result.get('latency_passed') is False or (acceptance.exists() and not json.loads(acceptance.read_text(encoding='utf-8'))['latency_and_reconciliation_passed']):
        name+=' (latency FAIL)'
    provenance.append({'file':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
    color=colors[index%len(colors)];rows=result['minutes'];xs=[];ys=[]
    for row in rows:
        if row['source']=='cam0' and row['metrics'].get('input_decode',{}).get('count'):
            xs.append(row['minute']+.5);ys.append(row['metrics']['input_decode']['p99'])
    axes[0].plot(xs,ys,color=color,label=name,lw=1.6)
    for axis,metric,label in [(axes[1],'p99','P99'),(axes[2],'maximum','maximum')]:
        worst={}
        for row in rows:
            if not row['source'].startswith('cam') or row['minute']<10:continue
            current=row['metrics'].get('input_decode',{})
            baseline=result['baseline_5_10'].get(row['source'],{}).get('input_decode',{})
            if not current.get('count') or not baseline.get('count'):continue
            minute=row['minute'];delta=current[metric]-baseline[metric]
            worst[minute]=max(worst.get(minute,float('-inf')),delta)
        axis.plot([m+.5 for m in sorted(worst)],[worst[m] for m in sorted(worst)],color=color,label=name,lw=1.6)

axes[0].set_ylabel('cam0 Input to Decode\nP99 (ms)')
axes[1].set_ylabel('Worst camera P99 growth\nrelative to minutes 5–10 (ms)')
axes[2].set_ylabel('Worst camera max growth\nrelative to minutes 5–10 (ms)')
axes[1].axhline(1000/30,color='#333333',ls='--',lw=1,label='provisional limit 33.33 ms')
axes[2].axhline(2000/30,color='#333333',ls='--',lw=1,label='provisional limit 66.67 ms')
for axis in axes:
    axis.axvspan(5,10,color='#dce7f0',alpha=.6)
    axis.grid(alpha=.2);axis.set_xlim(0,62);axis.legend(loc='upper right',fontsize=8)
    axis.axvline(880.11/60,color=colors[0],ls=':',alpha=.8)
axes[0].text(15.1,.95,'Baseline: ReID/NPU/save stopped at ~14.7 min.\nLater low latency is not healthy operation.',transform=axes[0].get_xaxis_transform(),va='top',fontsize=8,color=colors[0])
axes[2].set_xlabel('Elapsed diagnostic minutes (partial final minute included)')
fig.suptitle('Input to Decode: exact per-frame minute distributions\nUbuntu 24.04 / gfx1150, unchanged 15-video + resource wall',fontsize=12)
a.output.parent.mkdir(parents=True,exist_ok=True);fig.savefig(a.output,dpi=160)
a.output.with_suffix('.provenance.json').write_text(json.dumps({'inputs':provenance,'script_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'python':sys.version,'matplotlib':matplotlib.__version__},indent=2)+'\n')
