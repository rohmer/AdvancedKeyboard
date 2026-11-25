#!/usr/bin/env python3
"""Measure legacy and optimized native trainers on an identical prepared split."""
from __future__ import annotations
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

import benchmark as bench


def snapshot(executable: Path, directory: Path):
    digest = bench.digest_file(executable)
    target = directory / digest / executable.name
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists(): shutil.copy2(executable, target)
    if bench.digest_file(target) != digest: raise RuntimeError('Executable snapshot mismatch')
    return target.resolve()


def index_model(model: Path, builder: Path):
    index = model.with_suffix('.ngix')
    if index.exists(): return index, 0.0
    if struct.unpack_from('<I', model.read_bytes(), 4)[0] != 3:
        raise RuntimeError('Native trainer did not emit the required versioned index')
    started = time.perf_counter()
    subprocess.run([sys.executable,str(builder),str(model),str(index)],check=True,
                   stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    return index,time.perf_counter()-started


def predict(probe: Path, model: Path, index: Path, queries: Path, output: Path):
    outcomes = output.with_name(output.stem+'-outcomes.tsv')
    command = [str(probe),str(model),str(index),str(queries),str(outcomes)]
    result = subprocess.run(command,capture_output=True,text=True,encoding='utf-8',check=True,timeout=1800,
                            creationflags=subprocess.CREATE_NO_WINDOW if sys.platform=='win32' else 0)
    output.write_text(result.stdout,encoding='utf-8')
    data = json.loads(result.stdout)
    data.update(probeSHA256=bench.digest_file(probe),queriesSHA256=bench.digest_file(queries),
                outcomesSHA256=bench.digest_file(outcomes))
    output.write_text(json.dumps(data,indent=2)+'\n',encoding='utf-8')
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepared',type=Path,required=True)
    parser.add_argument('--baseline',type=Path,required=True)
    parser.add_argument('--optimized',type=Path)
    parser.add_argument('--baseline-probe',type=Path,required=True)
    parser.add_argument('--optimized-probe',type=Path)
    parser.add_argument('--wordlists',type=Path,required=True)
    parser.add_argument('--index-builder',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--variants',default='baseline,optimized-compact,optimized-radix,optimized-smoothed')
    parser.add_argument('--workers',type=int,default=4)
    parser.add_argument('--training-memory-mib',type=int,default=512)
    parser.add_argument('--hard-memory-mib',type=int,default=2048)
    parser.add_argument('--timeout',type=float,default=21600)
    parser.add_argument('--subset',action='store_true')
    parser.add_argument('--primary-size',type=int,default=262144)
    parser.add_argument('--small-size',type=int,default=16384)
    parser.add_argument('--skip-warm',action='store_true')
    parser.add_argument('--evaluation-split',choices=('validation','test'),default='test')
    args = parser.parse_args()
    manifest_path = args.prepared/'manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    training = [Path(p) for p in manifest['subsetFiles' if args.subset else 'trainingFiles']]
    if not all(p.is_file() for p in training): raise ValueError('Prepared training split is incomplete')
    args.output.mkdir(parents=True,exist_ok=True)
    variants = args.variants.split(',')
    report = {'version':1,'scope':'hash-sampled subset' if args.subset else 'full prepared training split',
              'preparationScopeLimitation':manifest.get('scopeLimitation'),
              'evaluationSplit':args.evaluation_split,
              'corpusManifestSHA256':bench.digest_file(manifest_path),'trainingBytes':sum(p.stat().st_size for p in training),
              'workers':args.workers,'trainingMemoryMiB':args.training_memory_mib,
              'hardMemoryMiB':args.hard_memory_mib,'runs':[]}
    for variant in variants:
        if variant not in ('baseline','optimized-compact','optimized-radix','optimized-smoothed'):
            raise ValueError('Unknown variant '+variant)
        legacy = variant=='baseline'
        batch = variant in ('optimized-radix','optimized-smoothed')
        source_executable = args.baseline if legacy else args.optimized
        source_probe = args.baseline_probe if legacy else args.optimized_probe
        if not source_executable or not source_probe: raise ValueError('Optimized executable and probe required')
        executable = snapshot(source_executable.resolve(),args.output/'executables')
        probe = snapshot(source_probe.resolve(),args.output/'executables')
        variant_output = args.output/variant
        cache = variant_output/'cache'
        scratch = variant_output/'scratch'
        if cache.exists() and any(cache.iterdir()): raise ValueError('Cold cache must be empty: '+str(cache))
        # The legacy CLI exports one profile per invocation. Optimized batch
        # output produces primary256 plus16 from a single training phase.
        schedule = [('cold',args.primary_size)]
        if not args.skip_warm: schedule.append(('warm',args.primary_size))
        if not batch: schedule.append(('profile-reuse',args.small_size))
        models = {}
        for temperature,size in schedule:
            output = variant_output/(temperature+'-'+str(size))
            output.mkdir(parents=True,exist_ok=True)
            model = output/'model.ngtr'
            command = [str(executable),'--language','fr','--workers',str(args.workers),'--low-memory',
                       '--trie-size',str(size),'--trie-output',str(model.resolve()),
                       '--wordlist-dir',str(args.wordlists.resolve()),'--cache-directory',str(cache.resolve()),
                       '--scratch-directory',str(scratch.resolve())]
            if legacy: command.append('--compact')
            else:
                command.extend(['--memory-mib',str(args.training_memory_mib),'--format',
                                'compact' if variant=='optimized-compact' else 'radix',
                                '--smoothing','modified-kn' if variant=='optimized-smoothed' else 'counts'])
                if batch: command.extend(['--profile-sizes',str(args.small_size)])
            command += ['--',*(str(p.resolve()) for p in training)]
            print(json.dumps({'stage':'generation-start','variant':variant,'temperature':temperature,
                              'profileBytes':size,'output':str(output.resolve())}),flush=True)
            result = bench.run_process(command,output,args.hard_memory_mib*1024*1024,args.timeout)
            index,index_seconds = index_model(model,args.index_builder.resolve())
            result.update(variant=variant,temperature=temperature,profileBytes=size,
                          indexBuildSeconds=index_seconds,artifactReadySeconds=result['wallSeconds']+index_seconds,
                          modelBytes=model.stat().st_size,indexBytes=index.stat().st_size,
                          combinedModelBytes=model.stat().st_size+index.stat().st_size,
                          modelSHA256=bench.digest_file(model),indexSHA256=bench.digest_file(index))
            if size in models:
                if result['modelSHA256']!=models[size][0] or result['indexSHA256']!=models[size][1]:
                    raise RuntimeError('Cold/warm output mismatch: '+variant)
            models[size]=(result['modelSHA256'],result['indexSHA256'])
            result['quality']=predict(probe,model,index,args.prepared/(args.evaluation_split+'.tsv'),output/'quality.json')
            if batch:
                small=model.with_name(model.stem+'-'+str(args.small_size)+model.suffix)
                if not small.exists(): raise RuntimeError('Optimized trainer did not emit batch profile '+str(small))
                small_index,small_index_seconds=index_model(small,args.index_builder.resolve())
                result['batchProfile']={'bytes':small.stat().st_size+small_index.stat().st_size,
                    'modelSHA256':bench.digest_file(small),'indexSHA256':bench.digest_file(small_index),
                    'indexBuildSeconds':small_index_seconds,
                    'quality':predict(probe,small,small_index,args.prepared/(args.evaluation_split+'.tsv'),output/'quality-small.json')}
            report['runs'].append(result)
            (args.output/'results.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
            print(json.dumps({'stage':'generation-complete','variant':variant,'temperature':temperature,
                              'seconds':result['artifactReadySeconds'],'peakBytes':result['peakMemoryBytes'],
                              'modelAndIndexBytes':result['combinedModelBytes']}),flush=True)
    print(json.dumps({'stage':'matrix-complete','runs':len(report['runs'])}),flush=True)


if __name__=='__main__': main()
