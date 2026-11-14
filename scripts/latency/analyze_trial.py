"""Bounded-memory frame reconciliation and exact per-minute latency distributions."""
import argparse,collections,csv,gzip,json,math,statistics,hashlib,shutil
from array import array
from pathlib import Path
EDGES={'input_au':('input','access_unit'),'au_decoder':('access_unit','decode_input'),
       'decoder':('decode_input','decode_output'),'input_decode':('input','decode_output'),
       'decode_handoff':('decode_output','handoff'),'decode_preview':('decode_output','preview_ready'),
       'preview_draw':('preview_ready','draw'),'draw_swap':('draw','swap'),
       'input_swap':('input','swap'),'ai_total':('ai_submit','ai_complete')}
STAGE_EVENTS={s for pair in EDGES.values() for s in pair}|{'decode_submit','latest_replaced','preview_discarded','preview_not_requested','background_latest_released','ingest_retired','ingest_discarded','shutdown_latest_retired','shutdown_preview_retired','decode_preroll','decode_dispatch_retired','rtp_assembly_retired','receive_au_closed'}
EDGES_BY_TARGET=collections.defaultdict(list)
for name,(a,b) in EDGES.items():EDGES_BY_TARGET[b].append((name,a))
def distribution(values):
    values=sorted(values); n=len(values)
    if not n:return {'count':0}
    return {'count':n,'mean':statistics.mean(values),'p99':values[max(0,math.ceil(n*.99)-1)],'maximum':values[-1]}
def analyze(directory):
    directory=Path(directory); path=directory/'diagnostics/events.csv'
    opener=open
    if not path.exists():path=Path(str(path)+'.gz');opener=gzip.open
    stage_ranges=collections.defaultdict(dict);arrival={};previous_decode={};frames={};buckets=collections.defaultdict(lambda:collections.defaultdict(lambda:array('d')))
    unclassified_examples=[]
    counts=collections.Counter();incomplete=collections.Counter();stagecounts=collections.defaultdict(collections.Counter)
    first=None;last=0;next_prune=0;records_read=0;previous_pts={};pts_anomalies=[];generations=collections.defaultdict(set);shutdown_ns=None;retired_ns=[]
    def retire(key,frame):
        source,gen,seq=key
        if 'rtp_assembly_retired' in frame and 'input' in frame and (shutdown_ns is None or frame['input']<shutdown_ns):counts['pre_shutdown_rtp_unassembled']+=1
        if 'rtp_assembly_retired' in frame and any(k in frame for k in ['access_unit','decode_submit','decode_output']):counts['invalid_rtp_retirement']+=1
        if 'input' in frame and 'decode_submit' not in frame:
            reason='shutdown_au_retired' if 'receive_au_closed' in frame and 'access_unit' in frame and shutdown_ns is not None and frame.get('input',0)>=shutdown_ns and frame['receive_au_closed']>=frame['access_unit'] else 'partial_rtp_retired' if 'rtp_assembly_retired' in frame and 'access_unit' not in frame and shutdown_ns is not None and frame.get('input',0)>=shutdown_ns else 'ingest_retired' if 'ingest_retired' in frame else 'decode_dispatch_retired' if 'decode_dispatch_retired' in frame else 'decode_preroll' if 'decode_preroll' in frame else 'ingest_discarded' if 'ingest_discarded' in frame else 'input_not_submitted'
            incomplete[(source,reason)]+=1
        if 'ai_submit' in frame and 'ai_complete' not in frame:incomplete[(source,'ai_submitted_no_completion')]+=1
        if 'decode_submit' in frame and 'decode_output' not in frame:incomplete[(source,'submitted_no_output')]+=1
        if 'decode_output' in frame and 'draw' not in frame:
            reason='shutdown_retired' if 'shutdown_latest_retired' in frame or 'shutdown_preview_retired' in frame else 'latest_replaced' if 'latest_replaced' in frame else 'preview_discarded' if 'preview_discarded' in frame else 'preview_not_requested' if 'preview_not_requested' in frame else 'background_latest_released' if 'background_latest_released' in frame else 'unclassified_not_drawn'
            incomplete[(source,reason)]+=1
            if reason=='unclassified_not_drawn' and len(unclassified_examples)<128:
                unclassified_examples.append(dict(source=source,generation=gen,sequence=seq,stages=dict(frame)))
        if 'draw' in frame and 'swap' not in frame:incomplete[(source,'drawn_no_swap')]+=1
    with opener(path,'rt',newline='') as stream:
        reader=csv.DictReader(stream,escapechar='\\',doublequote=False)
        for row in reader:
            records_read+=1
            ns=int(row['ns']);source=row['source'];event=row['event'];seq=int(row['sequence']);gen=int(row['generation'])
            v=int(row['value']);v2=int(row['value2'])
            if source=='ui-slot':source='ui-slot-'+str(seq-1)
            if first is None:first=ns;next_prune=ns+30_000_000_000
            last=max(last,ns);minute=max(0,(ns-first)//60_000_000_000);bucket=buckets[(source,minute)]
            counts[event]+=1
            if event=='decoder_drain' and v2:counts['decoder_drain_failed']+=1
            if event=='shutdown_requested':shutdown_ns=ns
            if event in ['ingest_retired','shutdown_latest_retired','shutdown_preview_retired','decode_dispatch_retired','rtp_assembly_retired']:
                retired_ns.append(ns)
            if event=='pts' and int(row['pts'])>=0:
                pts=int(row['pts']);stream=(source,gen,v)
                old=previous_pts.get(stream)
                generations[source].add(gen)
                if old is not None and pts<old:
                    counts['pts_backwards_observed']+=1
                    if len(pts_anomalies)<1000:
                        pts_anomalies.append(dict(source=source,generation=gen,mode=v,sequence=seq,ns=ns,previous=old,pts=pts))
                previous_pts[stream]=pts
            if seq and event in STAGE_EVENTS:
                key=(source,gen,seq);frame=frames.setdefault(key,{'last':ns})
                frame['last']=max(frame['last'],ns)
                if event not in frame:
                    frame[event]=ns;stagecounts[source][event]+=1
                    limits=stage_ranges[source].setdefault(event,[ns,ns]);limits[0]=min(limits[0],ns);limits[1]=max(limits[1],ns)
                    if event=='decode_output' and 'input' not in frame:counts['output_without_input_identity']+=1
                    if event=='decode_output':
                        previous=previous_decode.get((source,gen))
                        if previous is not None and ns>=previous:bucket['decode_interval_ms'].append((ns-previous)/1e6)
                        previous_decode[(source,gen)]=ns
                        period=100000000 if source=='resource-monitor' else 1e9/30
                        state=arrival.setdefault((source,gen),{'first_ns':ns,'count':0,'maximum_deficit_frames':0})
                        state['count']+=1
                        state['maximum_deficit_frames']=max(state['maximum_deficit_frames'],(ns-state['first_ns'])/period-(state['count']-1))
                    for name,a in EDGES_BY_TARGET.get(event,[]):
                        if a in frame:
                            delta=ns-frame[a]
                            if delta>=0:bucket[name].append(delta/1e6)
                            else:counts['negative_'+name]+=1
            if event.startswith(('control_','catalog_')) and v>=0:
                bucket[event+'_ms'].append(v/1e6)
            if event in ['decoded_lock','handoff','preview_scale','appsrc_time','ingest_oldest_age','decode_dispatch_wait','decode_dispatch_oldest'] and v>=0:
                bucket[event+'_ms'].append(v/1e6)
            if event in ['preview_lock','ui_select_lock','ui_draw_lock']:
                bucket[event+'_wait_ms'].append(v/1e6);bucket[event+'_hold_ms'].append(v2/1e6)
            if event in ['ingest_queue','appsrc_queue','decode_dispatch_queue']:
                bucket[event+'_bytes'].append(v);bucket[event+'_frames'].append(v2)
            if ns>=next_prune:
                for key in list(frames):
                    if ns-frames[key]['last']>30_000_000_000:retire(key,frames.pop(key))
                next_prune=ns+10_000_000_000
    for key,frame in frames.items():retire(key,frame)
    if first is None:raise ValueError('No diagnostic events')
    baselines=collections.defaultdict(dict)
    for (source,minute),metrics in buckets.items():
        if 5<=minute<10:
            for name,values in metrics.items():baselines[source].setdefault(name,array('d')).extend(values)
    summarypath=directory/'diagnostics/diagnostics-summary.json'
    integrity=json.loads(summarypath.read_text()) if summarypath.exists() else None
    result={'schema':1,'first_ns':first,'duration_s':(last-first)/1e9,'events':dict(counts),
            'records_read':records_read,
            'shutdown_ns':shutdown_ns,
            'retirements_before_shutdown':sum(1 for ns in retired_ns if shutdown_ns is None or ns<shutdown_ns),
            'diagnostic_integrity':integrity,
            'diagnostic_integrity_valid':bool(integrity and integrity['accepted']==integrity['written']==records_read and
                not any(integrity[k] for k in ['queue_lost','limit_lost','pending','write_failed'])),
            'analyzer_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'stage_ranges_ns':dict(stage_ranges),
            'baseline_5_10':{source:{name:distribution(values) for name,values in metrics.items()} for source,metrics in baselines.items()},
            'decode_arrival_deficit':[dict(source=s,generation=g,**v,minimum_prefill_frames=math.ceil(v['maximum_deficit_frames'])) for (s,g),v in sorted(arrival.items())],
            'generations':{s:sorted(v) for s,v in generations.items()},
            'pts_backwards_examples':pts_anomalies,
            'pts_note':'Backward PTS is observed per probe mode and generation; codec reordering and new generations must be reviewed separately.',
            'arrival_note':'Logical decoder pad output; not proof of VA hardware completion or lossless presentation.',
            'unclassified_examples':unclassified_examples,'stages':dict(stagecounts),'incomplete':[{'source':s,'kind':k,'count':n} for (s,k),n in sorted(incomplete.items())],
            'minutes':[{'source':s,'minute':m,'metrics':{k:distribution(v) for k,v in values.items()}}
                       for (s,m),values in sorted(buckets.items())]}
    shutil.copyfile(Path(__file__),directory/'analyzer-used.py')
    (directory/'frame-analysis.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k not in ['minutes','stages','stage_ranges_ns','baseline_5_10','decode_arrival_deficit']},indent=2))
    for source in ['cam0','cam7','cam14']:
        rows=[r for r in result['minutes'] if r['source']==source and 'input_decode' in r['metrics']]
        print(source,[(r['minute'],round(r['metrics']['input_decode']['mean'],2),round(r['metrics']['input_decode']['p99'],2)) for r in rows])
    return result
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('directory');analyze(p.parse_args().directory)
