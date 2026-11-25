#!/usr/bin/env python3
"""Measure frozen native runtimes in balanced order on identical model/query bytes."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import statistics

import benchmark as bench
import matrix


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime',action='append',required=True,metavar='LABEL=EXECUTABLE')
    parser.add_argument('--models',type=Path,required=True,help='Frozen model manifest from this task')
    parser.add_argument('--queries',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--repetitions',type=int,default=8)
    parser.add_argument('--cpu',type=int,default=2)
    parser.add_argument('--timeout',type=float,default=1200)
    parser.add_argument('--hard-memory-mib',type=int,default=256)
    parser.add_argument('--only',default='',help='Comma-separated variant:size keys')
    args=parser.parse_args()
    if args.repetitions<2:raise ValueError('At least two measured repetitions are required')
    args.output.mkdir(parents=True,exist_ok=True)
    runtimes={}
    for item in args.runtime:
        label,separator,path=item.partition('=')
        if not separator or not label or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-' for c in label):
            raise ValueError('Use LABEL=EXECUTABLE with a simple label')
        if label in runtimes:raise ValueError('Duplicate runtime label')
        runtimes[label]=matrix.snapshot(Path(path).resolve(),args.output/'executables')
    models=json.loads(args.models.read_text(encoding='utf-8'))
    requested=set(args.only.split(',')) if args.only else None
    models=[m for m in models if requested is None or f"{m['variant']}:{m['profileBytes']}" in requested]
    if not models:raise ValueError('No models selected')
    for model in models:
        for field,digest in (('model','modelSHA256'),('index','indexSHA256')):
            if bench.digest_file(Path(model[field]))!=model[digest]:raise ValueError('Model snapshot hash mismatch')
    report={'version':1,'queriesSHA256':bench.digest_file(args.queries),
            'modelsManifestSHA256':bench.digest_file(args.models),
            'repetitions':args.repetitions,'pinnedLogicalCPU':args.cpu,
            'hardMemoryMiB':args.hard_memory_mib,
            'methodology':'Each native process preloads and normalizes all queries, performs one complete warm-up pass, then one measured pass. Runtime order rotates and reverses between repetitions; wall clock includes process startup and is not lookup latency. C++ allocation counts cover the native call only, not query/probe/model loading. No OS file-cache flush.',
            'runs':[],'summaries':[]}
    labels=list(runtimes)
    for model in models:
        for repetition in range(args.repetitions):
            rotation=(repetition//2)%len(labels)
            order=labels[rotation:]+labels[:rotation]
            if repetition%2:order=list(reversed(order))
            for label in order:
                output=args.output/model['variant']/str(model['profileBytes'])/label
                output.mkdir(parents=True,exist_ok=True)
                command=[str(runtimes[label]),model['model'],model['index'],str(args.queries.resolve()),str(args.cpu)]
                native=bench.run_process(command,output/f'run-{repetition}',args.hard_memory_mib*1024*1024,args.timeout)
                measured=json.loads((output/f'run-{repetition}'/'stdout.log').read_text(encoding='utf-8'))
                record={'runtime':label,'variant':model['variant'],'profileBytes':model['profileBytes'],
                        'repetition':repetition,'order':order,'command':command,
                        'executableSHA256':bench.digest_file(runtimes[label]),
                        'wallSeconds':native['wallSeconds'],'peakMemoryBytes':native['peakMemoryBytes'],
                        'memoryMetric':native['memoryMetric'],'measurement':measured}
                (output/f'run-{repetition}.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
                report['runs'].append(record)
                (args.output/'results.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
                print(json.dumps({'runtime':label,'variant':model['variant'],'profileBytes':model['profileBytes'],
                                  'repetition':repetition,'wallSeconds':record['wallSeconds']}),flush=True)
        for label in labels:
            selected=[r['measurement'] for r in report['runs'] if r['runtime']==label and r['variant']==model['variant'] and r['profileBytes']==model['profileBytes']]
            if len({(r['completion_fingerprint'],r['correction_fingerprint']) for r in selected})!=1:
                raise ValueError('Runtime outputs are not deterministic across repetitions')
            summary={'runtime':label,'variant':model['variant'],'profileBytes':model['profileBytes'],
                     'completionFingerprint':selected[0]['completion_fingerprint'],
                     'correctionFingerprint':selected[0]['correction_fingerprint'],
                     'modelObjectBytes':selected[0]['model_object_bytes'],
                     'cursorObjectBytes':selected[0]['cursor_object_bytes'],
                     'prefixScratchBytes':selected[0]['prefix_scratch_bytes'],
                     'correctionScratchBytes':selected[0]['correction_scratch_bytes'],'byKind':[]}
            summary['nativeProcessPeakMemoryBytes']=max(r['peakMemoryBytes'] for r in report['runs'] if r['runtime']==label and r['variant']==model['variant'] and r['profileBytes']==model['profileBytes'])
            for kind in range(6):
                samples=[r['by_kind'][kind] for r in selected]
                summary['byKind'].append({'kind':('p0','p1','p2','p3','clean','delete')[kind],
                    'queriesPerRun':samples[0]['queries'],
                    'p50MedianUs':statistics.median(r['p50_us'] for r in samples),
                    'p95MedianUs':statistics.median(r['p95_us'] for r in samples),
                    'p50MinMaxUs':[min(r['p50_us'] for r in samples),max(r['p50_us'] for r in samples)],
                    'p95MinMaxUs':[min(r['p95_us'] for r in samples),max(r['p95_us'] for r in samples)],
                    'cppAllocationCalls':sum(r['cpp_allocation_calls'] for r in samples)})
            report['summaries'].append(summary)
        (args.output/'results.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'stage':'runtime-matrix-complete','runs':len(report['runs'])}),flush=True)


if __name__=='__main__':main()
