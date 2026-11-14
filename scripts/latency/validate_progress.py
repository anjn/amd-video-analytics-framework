"""Report persistent errors and AI stalls separately from YOLO frame completion."""
import argparse,json,hashlib
from pathlib import Path

def validate_progress(directory):
    directory=Path(directory);manifest=json.loads((directory/'manifest.json').read_text())
    start=manifest['start_monotonic_ns'];required=['YOLO inference','ReID inference + transfer','Image embedding NPU','PNG + DB save']
    previous={};first={};last={};gaps={};errors={};snapshots=0;last_ns=None;snapshot_gap=0
    channel_progress={};visible=set();final_states=[]
    with (directory/'diagnostics/status.jsonl').open() as stream:
        for line in stream:
            row=json.loads(line);ns=row['ns'];elapsed=(ns-start)/1e9;snapshots+=1
            if last_ns is not None:snapshot_gap=max(snapshot_gap,(ns-last_ns)/1e9)
            last_ns=ns
            for metric in row.get('ai',[]):
                if metric['camera']:continue
                key=metric['stage'];count=metric['completed']
                if count>previous.get(key,0):
                    first.setdefault(key,elapsed)
                    if key in last:gaps[key]=max(gaps.get(key,0),elapsed-last[key]['elapsed_s'])
                    last[key]={'elapsed_s':elapsed,'completed':count}
                previous[key]=count
            for channel in row['channels']:
                if 'decoded' not in channel:continue # Legacy diagnostic schema.
                key=channel['id'];item=channel_progress.setdefault(key,{'previous':0,'first':None,'last':None,'gap':0,'last_display':None})
                count=channel['decoded']
                if elapsed>=30 and channel.get('slot',-1)>=0:visible.add(key)
                if count>item['previous']:
                    if item['first'] is None:item['first']=elapsed
                    if item['last'] is not None:item['gap']=max(item['gap'],elapsed-item['last'])
                    item['last']=elapsed
                item['previous']=count
                if channel.get('display_fps',0)>0:item['last_display']=elapsed
            states=[('fatal',row.get('fatal',''))]
            for key in ['error','tracking_error']:
                states.append(('search.'+key,row.get('search',{}).get(key,'')))
            for channel in row['channels']:
                states.append((channel['id'],channel['error']))
            final_states=states
            if elapsed>=120:
                for key,error in states:
                    if error:errors.setdefault((key,error),elapsed)
    # A short pilot must not pass merely because its final sample precedes
    # the long-run warmup boundary. Require its terminal state to be clean too.
    if last_ns is not None and (last_ns-start)/1e9>=60:
        for key,error in final_states:
            if error:errors.setdefault((key,error),(last_ns-start)/1e9)
    failures=[{'stage':key,'error':error,'first_elapsed_s':elapsed} for (key,error),elapsed in errors.items()]
    end=(last_ns-start)/1e9 if last_ns is not None else 0
    for key in required:
        if key not in last:failures.append({'stage':key,'reason':'no progress recorded'});continue
        gap=max(gaps.get(key,0),end-last[key]['elapsed_s'])
        if gap>30:failures.append({'stage':key,'reason':'progress gap above 30 seconds','seconds':gap})
    if end>=60:
        for key,item in channel_progress.items():
            if key in visible and item['first'] is None:
                failures.append({'stage':key,'reason':'visible source has no decoded progress'})
            if item['last'] is not None and max(item['gap'],end-item['last'])>30:
                failures.append({'stage':key,'reason':'decode progress gap above 30 seconds'})
            if key in visible and (item['last_display'] is None or end-item['last_display']>30):
                failures.append({'stage':key,'reason':'visible source has no recent presentation'})
    if snapshot_gap>15:failures.append({'reason':'status snapshot gap above 15 seconds','seconds':snapshot_gap})
    result={'validation_schema':2,'validator_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'progress_passed':not failures,'snapshots':snapshots,'duration_s':end,'largest_snapshot_gap_s':snapshot_gap,
            'channel_progress':channel_progress,'visible_sources':sorted(visible),'first_progress':first,'last_progress':last,'largest_progress_gap_s':gaps,'failures':failures}
    (directory/'progress-validation.json').write_text(json.dumps(result,indent=2)+'\n')
    return result

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('directory',type=Path)
    result=validate_progress(parser.parse_args().directory)
    print(json.dumps({key:result[key] for key in ['progress_passed','snapshots','failures']},indent=2))
