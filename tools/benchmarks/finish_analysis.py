#!/usr/bin/env python3
"""Retain format-only parity and paired held-out comparisons after measurement."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import subprocess

import benchmark as bench
import compare
import matrix


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def quality_path(run, small=False):
    model = Path(run['command'][run['command'].index('--trie-output')+1])
    return model.parent/('quality-small.json' if small else 'quality.json')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepared',type=Path,required=True)
    parser.add_argument('--baseline',type=Path,required=True)
    parser.add_argument('--radix',type=Path,required=True)
    parser.add_argument('--equal-storage',type=Path,required=True)
    parser.add_argument('--converter',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    baseline,radix,equal=map(read,(args.baseline,args.radix,args.equal_storage))
    corpus=bench.digest_file(args.prepared/'manifest.json')
    if any(report['corpusManifestSHA256']!=corpus for report in (baseline,radix,equal)):
        raise ValueError('Corpus manifests differ')
    query=bench.digest_file(args.prepared/'test.tsv')
    reference=next(r for r in radix['runs'] if r['temperature']=='cold')
    probe=args.radix.parent/'executables'/reference['quality']['probeSHA256']/'PredictionProbe.exe'
    if not probe.exists():probe=probe.with_suffix('')
    if bench.digest_file(probe)!=reference['quality']['probeSHA256']:
        raise ValueError('Frozen prediction probe differs')
    converter=matrix.snapshot(args.converter.resolve(),args.output/'executables')
    args.output.mkdir(parents=True,exist_ok=True)
    conversions={'corpusManifestSHA256':corpus,'queriesSHA256':query,
                 'converterSHA256':bench.digest_file(converter),'profiles':[]}
    baseline_profiles={r['profileBytes']:r for r in baseline['runs'] if r['temperature']!='warm'}
    for size,run in sorted(baseline_profiles.items(),reverse=True):
        source=Path(run['command'][run['command'].index('--trie-output')+1])
        output=args.output/'format-only'/str(size)
        output.mkdir(parents=True,exist_ok=True)
        model=output/'model.ngtr'
        command=[str(converter),str(source),str(source.with_suffix('.ngix')),str(model)]
        converted=subprocess.run(command,capture_output=True,text=True,encoding='utf-8',check=True)
        native=matrix.predict(probe,model,model.with_suffix('.ngix'),args.prepared/'test.tsv',output/'quality.json')
        paired=compare.compare(quality_path(run),output/'quality.json')
        if paired['orderedOutputFingerprintMismatches']!=0:
            raise ValueError('Format-only conversion changed held-out outputs')
        conversions['profiles'].append({'nominalProfileBytes':size,
            'sourceCombinedBytes':run['combinedModelBytes'],
            'targetCombinedBytes':model.stat().st_size+model.with_suffix('.ngix').stat().st_size,
            'sourceModelSHA256':bench.digest_file(source),
            'sourceIndexSHA256':bench.digest_file(source.with_suffix('.ngix')),
            'modelSHA256':bench.digest_file(model),'indexSHA256':bench.digest_file(model.with_suffix('.ngix')),
            'command':command,'conversion':json.loads(converted.stdout),'quality':native,'comparison':paired})
        print(json.dumps({'formatOnlyProfile':size,'fingerprintMismatches':paired['orderedOutputFingerprintMismatches']}),flush=True)
    (args.output/'format-only.json').write_text(json.dumps(conversions,indent=2)+'\n',encoding='utf-8')

    comparisons={'corpusManifestSHA256':corpus,'queriesSHA256':query,'comparisons':[]}
    def append(label,left,right):
        result=compare.compare(left,right)
        comparisons['comparisons'].append({'label':label,'comparison':result})
        print(json.dumps({'comparison':label,'top3Delta':result['metrics']['completionTop3']['delta']}),flush=True)
    cold={r['variant']:r for r in radix['runs'] if r['temperature']=='cold'}
    for size in (262144,16384):
        original=quality_path(baseline_profiles[size])
        small=size==16384
        for variant,run in cold.items():
            append(f'Original to {variant}, nominal {size//1024} KiB',original,quality_path(run,small))
        append(f'Radix counts to MKN, {size//1024} KiB combined',
               quality_path(cold['optimized-radix'],small),quality_path(cold['optimized-smoothed'],small))
        for run in equal['runs']:
            append(f'Original to {run["variant"]}, equal {run["batchProfile"]["budgetBytes"] if small else run["profileBytes"]} B storage',
                   original,quality_path(run,small))
    (args.output/'comparisons.json').write_text(json.dumps(comparisons,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__':main()
