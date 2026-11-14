"""Apply the documented provisional latency gates; preserve failed evidence."""
import argparse,json
from pathlib import Path

def latency_gates(result,period_ms=1000/30):
    failures=[];rows=[]
    for source in sorted(s for s in result['stages'] if s.startswith('cam')):
        stages=result['stages'][source]
        if not stages.get('decode_output'):continue
        reference=result['baseline_5_10'].get(source,{}).get('input_decode',{})
        if not reference.get('count'):
            failures.append({'source':source,'reason':'missing 5–10 minute baseline'});continue
        ranges=result.get('stage_ranges_ns',{}).get(source,{}).get('decode_output',[])
        coverage=(ranges[1]-ranges[0])/1e9 if len(ranges)==2 else 0
        if coverage<3600:failures.append({'source':source,'reason':'decode coverage below 60 minutes','seconds':coverage})
        for minute in result['minutes']:
            if minute['source']!=source or minute['minute']<10:continue
            metric=minute['metrics'].get('input_decode',{})
            if not metric.get('count'):continue
            # A partial final minute is reported too; it cannot hide an excursion.
            p99_growth=metric['p99']-reference['p99']
            max_growth=metric['maximum']-reference['maximum']
            passed=p99_growth<=period_ms and max_growth<=2*period_ms
            row={'source':source,'minute':minute['minute'],'count':metric['count'],
                 'p99_growth_ms':p99_growth,'max_growth_ms':max_growth,'passed':passed}
            rows.append(row)
            if not passed:failures.append(dict(row,reason='latency growth'))
    if not rows:failures.append({'reason':'no post-baseline latency data'})
    if not result['diagnostic_integrity_valid']:failures.append({'reason':'diagnostic record loss or missing clean summary'})
    if result['events'].get('ingest_discarded',0):
        failures.append({'reason':'encoded recording input discarded','count':result['events']['ingest_discarded']})
    for event,count in result['events'].items():
        if count and (event.startswith('negative_') or event in ['tracking_job_error','output_without_input_identity','decode_preroll','invalid_rtp_retirement','decoder_drain_failed','receive_drain_failed','pre_shutdown_rtp_unassembled']):
            failures.append({'reason':event,'count':count})
    if result.get('retirements_before_shutdown',0):failures.append({'reason':'frame retirement before controlled shutdown','count':result['retirements_before_shutdown']})
    for item in result['incomplete']:
        if item['kind'] in ['submitted_no_output','ai_submitted_no_completion','drawn_no_swap','unclassified_not_drawn','input_not_submitted','ingest_discarded','background_latest_released']:
            failures.append(dict(item,reason='incomplete frame reconciliation'))
    return {'latency_and_reconciliation_passed':not failures,'failures':failures,'minutes':rows,
            'note':'Requires separate AI/progress, kernel/copy audit and fixed-input review. Existing presentation discards are reported separately.'}

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('directory',type=Path)
    directory=parser.parse_args().directory
    result=latency_gates(json.loads((directory/'frame-analysis.json').read_text()))
    (directory/'acceptance.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'passed':result['latency_and_reconciliation_passed'],'failures':len(result['failures']),
                      'first_failures':result['failures'][:10]},indent=2))
