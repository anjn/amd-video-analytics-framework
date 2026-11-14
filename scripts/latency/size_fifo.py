"""Offline lower-bound FIFO sizing from existing metadata; no GPU/image access."""
import argparse,csv,gzip,hashlib,json,math
from array import array
from collections import defaultdict
from pathlib import Path

NANO=1_000_000_000

def arrival_envelope(times,fps=30):
    if fps<=0:raise ValueError('fps must be positive')
    if len(times)<2:return {'frames':len(times),'sufficient':False}
    origin=times[0];minimum=maximum=0;intervals=array('q');previous=origin
    for index,ns in enumerate(times):
        if ns<previous:raise ValueError('Producer monotonic time moved backwards')
        if index:intervals.append(ns-previous)
        phase=(ns-origin)*fps-index*NANO
        minimum=min(minimum,phase);maximum=max(maximum,phase);previous=ns
    # Ignore <=1ns representation rounding, then round the observed late phase
    # upward to a whole frame and add one frame of scheduling margin.
    prefill_frames=(max(0,maximum-fps)+NANO-1)//NANO+1
    delay=prefill_frames*NANO;capacity=0
    for index,ns in enumerate(times):
        relative=(ns-origin)*fps-delay
        # Equal-time arrival is counted before consumption: conservative capacity.
        consumed_before=max(0,(relative-1)//NANO+1)
        capacity=max(capacity,index+1-consumed_before)
    intervals=sorted(intervals);duration=times[-1]-origin
    return {'frames':len(times),'sufficient':True,'duration_s':duration/NANO,
        'observed_output_fps':(len(times)-1)*NANO/duration if duration else None,
        'interval_p99_ms':intervals[max(0,math.ceil(.99*len(intervals))-1)]/1e6,
        'maximum_interval_ms':intervals[-1]/1e6,
        'minimum_phase_ms':minimum/fps/1e6,'maximum_phase_ms':maximum/fps/1e6,
        'ending_phase_ms':((times[-1]-origin)*fps-(len(times)-1)*NANO)/fps/1e6,
        'initial_delay_frames':prefill_frames,'initial_delay_ms':prefill_frames*1000/fps,
        'capacity_frames_for_observed_arrivals':capacity,
        'note':'Includes one scheduling-margin frame and conservative tie ordering. Finite-trace estimate, not a guaranteed jitter bound or proof of VA completion.'}

def pts_summary(points,fps=30):
    gaps=backwards=duplicates=0;maximum=0
    for a,b in zip(points,points[1:]):
        delta=b-a
        if delta<0:backwards+=1
        elif delta==0:duplicates+=1
        elif delta*fps>3*NANO/2:gaps+=1
        maximum=max(maximum,delta)
    return {'samples':len(points),'backwards':backwards,'duplicates':duplicates,
        'intervals_above_1_5_periods':gaps,'maximum_interval_ms':maximum/1e6,
        'note':'PTS gaps need source/cadence review; they are not automatically declared packet or frame loss.'}

def size_fifo(directory,fps=30,skip_seconds=300):
    if fps<=0 or skip_seconds<0:raise ValueError('fps must be positive and warmup must be nonnegative')
    directory=Path(directory);summary_path=directory/'diagnostics/diagnostics-summary.json'
    if not summary_path.exists():raise ValueError('Missing clean diagnostic summary; cannot size FIFO from incomplete evidence')
    integrity=json.loads(summary_path.read_text())
    if integrity['accepted']!=integrity['written'] or any(integrity[k] for k in ['queue_lost','limit_lost','pending','write_failed']):
        raise ValueError('Diagnostic loss or write failure; cannot size FIFO from incomplete evidence')
    path=directory/'diagnostics/events.csv';opener=open
    if not path.exists():path=Path(str(path)+'.gz');opener=gzip.open
    streams=defaultdict(lambda:array('q'));points=defaultdict(lambda:array('q'))
    identities=defaultdict(lambda:array('Q'))
    first=None;rows=0
    with opener(path,'rt',newline='') as source:
        for row in csv.DictReader(source,escapechar='\\',doublequote=False):
            rows+=1;ns=int(row['ns'])
            if first is None:first=ns
            if ns<first+round(skip_seconds*NANO) or not row['source'].startswith('cam'):continue
            key=(row['source'],int(row['generation']));event=row['event']
            if event in ['decode_output','handoff']:
                sequence=int(row['sequence'])
                if sequence<=0:raise ValueError('Arrival has no valid frame identity')
                stream_key=key+(event,)
                streams[stream_key].append(ns);identities[stream_key].append(sequence)
            # PipelineTelemetry::Impl::Mode::DecoderOut is 4 in this schema.
            if event=='pts' and int(row['value'])==4 and int(row['pts'])>=0:points[key].append(int(row['pts']))
    if rows!=integrity['written']:raise ValueError('CSV row count does not match clean summary')
    results=[]
    for (source,generation,event),times in sorted(streams.items()):
        sequences=sorted(identities[(source,generation,event)])
        if any(a==b for a,b in zip(sequences,sequences[1:])):
            raise ValueError('Duplicate arrival frame identity; cannot infer producer cadence')
        results.append(dict(source=source,generation=generation,producer=event,
                            arrival=arrival_envelope(times,fps),pts=pts_summary(points[(source,generation)],fps)))
    if not results:raise ValueError("No eligible decoded-frame arrivals after the requested warmup")
    digest=hashlib.sha256()
    with path.open('rb') as data:
        for chunk in iter(lambda:data.read(2**20),b''):digest.update(chunk)
    result={'schema':1,'fps':fps,'skip_seconds':skip_seconds,'records_read':rows,
        'input_file':str(path),'input_sha256':digest.hexdigest(),
        'tool_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'streams':results,
        'limits':['Only observed pad-output/handoff arrival times; neither is a VA hardware-ready fence.',
                  'Existing latest replacement and presentation discards must be removed and throughput verified in the future FIFO phase.',
                  'PTS discontinuity, missing inputs or continuing phase drift require separate investigation; a FIFO cannot repair lost frames or insufficient sustained throughput.',
                  'VA pool capacity must also cover decoder DPB, in-flight consumers and allocator requirements; no pool size is changed here.']}
    (directory/'fifo-sizing.json').write_text(json.dumps(result,indent=2)+'\n');return result

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('directory',type=Path)
    parser.add_argument('--fps',type=int,default=30);parser.add_argument('--skip-seconds',type=int,default=300)
    args=parser.parse_args();r=size_fifo(args.directory,args.fps,args.skip_seconds)
    print(json.dumps({'streams':len(r['streams']),'output':str(args.directory/'fifo-sizing.json')},indent=2))
