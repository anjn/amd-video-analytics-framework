"""Strict physical-frame reconciliation, separate from Wayland commit counts.

Normal fixed-layout trials only. Explicit UI suspension is not an acceptance
escape: an interactive trial needs its own declared availability intervals.
Memory is bounded by the diagnostic time window, not total trial duration.
"""
import argparse, collections, csv, gzip, json
from pathlib import Path

DEFAULT_SOURCES = tuple('cam'+str(i) for i in range(15))+('resource-monitor',)
FRAME_EVENTS = {'input','decode_output','fifo_enter','fifo_exit','preview_ready',
                'draw','swap','presented','preview_not_requested',
                'shutdown_fifo_retired','shutdown_present_retired',
                'shutdown_preview_retired','shutdown_latest_retired',
                'presentation_commit_presented'}
FATAL_EVENTS = {'presentation_frame_lost','presentation_error','presentation_fifo_error',
                'preview_discarded','latest_replaced'}

def reconcile(rows, sources=DEFAULT_SOURCES, retire_age_ns=1_000_000_000):
    expected=set(sources); frames={}; first_admission={}; counts=collections.Counter()
    per_source=collections.defaultdict(collections.Counter); errors=collections.Counter()
    examples=[]; cutoff=None; watermark=0; next_prune=0; previous_pts={}; read=0
    def reject(reason,key=None):
        errors[reason]+=1
        if len(examples)<128:examples.append({'reason':reason,'frame':key})
    def finish(key,frame):
        source,gen,seq=key
        if 'decode_output' not in frame:
            if 'presented' in frame:reject('presented_without_decode',key)
            return
        per_source[source]['decoded']+=1
        if 'presented' in frame:
            if not all(e in frame for e in ['draw','swap','presentation_commit_presented']):
                reject('presented_without_submission_evidence',key)
            if frame.get('presentation_commit_presented')!=frame['presented']:
                reject('physical_timestamp_mismatch',key)
            if 'draw' in frame and frame['presented']<frame['draw']:
                reject('physical_timestamp_before_draw',key)
            per_source[source]['presented']+=1
            return
        retired=[frame[e] for e in ['shutdown_fifo_retired','shutdown_present_retired',
                 'shutdown_preview_retired','shutdown_latest_retired'] if e in frame]
        if retired and cutoff is not None and min(retired)>=cutoff and            cutoff-retire_age_ns<=frame['decode_output']<=cutoff+retire_age_ns and            max(retired)<=cutoff+retire_age_ns:
            per_source[source]['shutdown_retired']+=1;return
        startup=(source,gen) in first_admission and frame['decode_output']<first_admission[(source,gen)]
        if startup and 'preview_not_requested' in frame and not any(e in frame for e in ['fifo_enter','draw']):
            per_source[source]['startup_not_requested']+=1;return
        # Decoder drain after cutoff can yield frames which never enter the FIFO.
        if cutoff is not None and cutoff<=frame['decode_output']<=cutoff+retire_age_ns and            'preview_not_requested' in frame and not any(e in frame for e in ['fifo_enter','draw']):
            per_source[source]['shutdown_not_requested']+=1;return
        reject('decoded_frame_not_presented',key)
    for row in rows:
        read+=1;event=row['event'];source=row['source'];ns=int(row['ns'])
        watermark=max(watermark,ns);counts[event]+=1
        if event=='shutdown_requested':
            if cutoff is not None:reject('duplicate_shutdown_marker')
            cutoff=ns
        if event in FATAL_EVENTS and (source in expected or event in {'presentation_error','presentation_fifo_error'}):reject(event)
        if event=='presentation_mode' and int(row.get('value2',0))!=1:reject('unexpected_presentation_mode')
        seq=int(row.get('sequence',0));gen=int(row.get('generation',0))
        if source in expected and seq and event in FRAME_EVENTS:
            key=(source,gen,seq);frame=frames.setdefault(key,{'last':ns})
            frame['last']=max(frame['last'],ns)
            if event=='presented':
                if event in frame:reject('duplicate_unique_presentation',key)
                pts=int(row.get('pts',-1));old=previous_pts.get((source,gen))
                if pts>=0:
                    if old is not None and pts<=old:reject('presented_pts_not_increasing',key)
                    previous_pts[(source,gen)]=pts
            frame.setdefault(event,ns)
            if event=='fifo_enter':first_admission.setdefault((source,gen),ns)
        if watermark>=next_prune:
            for key in list(frames):
                if frames[key]['last']<watermark-10_000_000_000:finish(key,frames.pop(key))
            next_prune=watermark+1_000_000_000
    for key,frame in frames.items():finish(key,frame)
    if counts['presentation_mode']!=1:reject('missing_or_duplicate_physical_mode')
    if cutoff is None:reject('missing_shutdown_marker')
    for source in sorted(expected):
        if not per_source[source]['presented']:reject('source_never_presented',source)
    return {'physical_frames_passed':not errors,'records_read':read,'failures':dict(errors),
            'examples':examples,'sources':{s:dict(v) for s,v in sorted(per_source.items())},
            'events':dict(counts),'shutdown_ns':cutoff,
            'scope':'Fixed-layout normal trial; startup and <=1s shutdown dispositions are separate from physical presentation.'}

def validate(directory,sources=DEFAULT_SOURCES):
    directory=Path(directory);path=directory/'diagnostics/events.csv';opener=open
    if not path.exists():path=Path(str(path)+'.gz');opener=gzip.open
    with opener(path,'rt',newline='') as f:
        result=reconcile(csv.DictReader(f,escapechar=chr(92),doublequote=False),sources)
    summary=directory/'diagnostics/diagnostics-summary.json'
    integrity=json.loads(summary.read_text()) if summary.exists() else {}
    valid=integrity.get('accepted')==integrity.get('written')==result['records_read'] and           all(integrity.get(k,-1)==0 for k in ['queue_lost','limit_lost','pending','write_failed'])
    result['diagnostic_integrity_valid']=valid
    if not valid:result['failures']['diagnostic_record_loss_or_unclean_summary']=1
    result['physical_frames_passed']=not result['failures']
    (directory/'physical-frame-acceptance.json').write_text(json.dumps(result,indent=2)+'\n')
    return result

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('directory');args=p.parse_args()
    result=validate(args.directory)
    print(json.dumps(result,indent=2))
    raise SystemExit(0 if result['physical_frames_passed'] else 1)
