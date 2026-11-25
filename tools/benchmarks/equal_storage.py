#!/usr/bin/env python3
"""Compare radix count and smoothed profiles at the legacy actual flash cost."""
import argparse
import json
from pathlib import Path
import benchmark as bench
import matrix


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepared',type=Path,required=True)
    parser.add_argument('--baseline',type=Path,required=True)
    parser.add_argument('--radix',type=Path,required=True)
    parser.add_argument('--index-builder',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    baseline=json.loads(args.baseline.read_text(encoding='utf-8'))
    modern=json.loads(args.radix.read_text(encoding='utf-8'))
    if baseline['corpusManifestSHA256']!=modern['corpusManifestSHA256']:
        raise ValueError('Training/evaluation corpora differ')
    large=next(run for run in baseline['runs'] if run['temperature']=='cold')['combinedModelBytes']
    small=next(run for run in baseline['runs'] if run['profileBytes']==16384)['combinedModelBytes']
    args.output.mkdir(parents=True,exist_ok=True)
    report={'version':1,'scope':modern['scope'],'corpusManifestSHA256':modern['corpusManifestSHA256'],
            'evaluationSplit':modern['evaluationSplit'],'largeBudgetBytes':large,'smallBudgetBytes':small,
            'trainingBytes':modern['trainingBytes'],'runs':[]}
    for variant in ('optimized-radix','optimized-smoothed'):
        reference=next(run for run in modern['runs'] if run['variant']==variant and run['temperature']=='warm')
        output=args.output/variant
        model=output/'model.ngtr'
        command=reference['command'].copy()
        command[command.index('--trie-output')+1]=str(model.resolve())
        command[command.index('--trie-size')+1]=str(large)
        command[command.index('--profile-sizes')+1]=str(small)
        probe=args.radix.parent/'executables'/reference['quality']['probeSHA256']/'PredictionProbe.exe'
        if not probe.exists():
            probe=probe.with_suffix('')
        if not probe.exists():raise ValueError('Frozen native probe is missing')
        result=bench.run_process(command,output,modern['hardMemoryMiB']*1024*1024,21600)
        index,index_seconds=matrix.index_model(model,args.index_builder)
        small_model=model.with_name(model.stem+'-'+str(small)+model.suffix)
        small_index,_=matrix.index_model(small_model,args.index_builder)
        assert model.stat().st_size+index.stat().st_size<=large
        assert small_model.stat().st_size+small_index.stat().st_size<=small
        result.update(variant=variant,temperature='equal-storage-warm',profileBytes=large,
                      artifactReadySeconds=result['wallSeconds']+index_seconds,
                      combinedModelBytes=model.stat().st_size+index.stat().st_size,
                      modelSHA256=bench.digest_file(model),indexSHA256=bench.digest_file(index))
        result['quality']=matrix.predict(probe,model,index,args.prepared/'test.tsv',output/'quality.json')
        result['batchProfile']={'budgetBytes':small,'bytes':small_model.stat().st_size+small_index.stat().st_size,
            'modelSHA256':bench.digest_file(small_model),'indexSHA256':bench.digest_file(small_index),
            'quality':matrix.predict(probe,small_model,small_index,args.prepared/'test.tsv',output/'quality-small.json')}
        report['runs'].append(result)
        (args.output/'results.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
        print(json.dumps({'variant':variant,'seconds':result['artifactReadySeconds'],
                          'largeBytes':result['combinedModelBytes'],'smallBytes':result['batchProfile']['bytes']}),flush=True)


if __name__=='__main__':main()
