#!/usr/bin/env python3
"""Compare fixed before/after runtimes on validation, exposed regression and confirmation."""
from __future__ import annotations
import argparse
import json
from pathlib import Path

import benchmark as bench
import compare
import matrix


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline',type=Path,required=True)
    parser.add_argument('--candidate',type=Path,required=True)
    parser.add_argument('--models',type=Path,required=True)
    parser.add_argument('--prepared',type=Path,required=True)
    parser.add_argument('--confirmation',type=Path,required=True,help='Frozen confirmation directory')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    probes={label:matrix.snapshot(path.resolve(),args.output/'executables')
            for label,path in (('baseline',args.baseline),('candidate',args.candidate))}
    models=json.loads(args.models.read_text(encoding='utf-8'))
    panels={'validation':args.prepared/'validation.tsv','exposed-regression':args.prepared/'test.tsv',
            'confirmation':args.confirmation/'confirmation.tsv'}
    confirmation=json.loads((args.confirmation/'manifest.json').read_text(encoding='utf-8'))
    if bench.digest_file(panels['confirmation'])!=confirmation['confirmationQueries']['sha256']:
        raise ValueError('Confirmation query panel changed after freeze')
    for label,field in (('validation','validationQueriesSHA256'),('exposed-regression','oldTestQueriesSHA256')):
        if bench.digest_file(panels[label])!=confirmation[field]:
            raise ValueError('Previously frozen validation/regression panel changed')
    report={'version':1,'corpusManifestSHA256':bench.digest_file(args.prepared/'manifest.json'),
            'confirmationManifestSHA256':bench.digest_file(args.confirmation/'manifest.json'),
            'modelsManifestSHA256':bench.digest_file(args.models),
            'panelSHA256':{label:bench.digest_file(path) for label,path in panels.items()},
            'methodology':'Fixed model/index bytes and runtime policy. Validation used for development. Old test is exposed regression; disjoint confirmation was opened only after source/policy freeze. Incidental probe latency during platform builds is not an official timing measurement.',
            'runs':[]}
    if report['corpusManifestSHA256']!=confirmation['sourceCorpusManifestSHA256']:
        raise ValueError('Confirmation refers to a different prepared corpus')
    for panel,queries in panels.items():
        for model in models:
            if any(bench.digest_file(Path(model[key]))!=model[key+'SHA256'] for key in ('model','index')):
                raise ValueError('Frozen model bytes changed')
            output=args.output/panel/model['variant']/str(model['profileBytes'])
            output.mkdir(parents=True,exist_ok=True)
            quality={}
            for label,probe in probes.items():
                quality[label]=matrix.predict(probe,Path(model['model']),Path(model['index']),queries,output/(label+'.json'))
            paired=compare.compare(output/'baseline.json',output/'candidate.json')
            _,before=compare.load(output/'baseline.json');_,after=compare.load(output/'candidate.json')
            completion_mismatches=sum(a['outputFingerprint']!=b['outputFingerprint'] for a,b in zip(before,after) if a['kind'].startswith('p'))
            if completion_mismatches:raise ValueError('Runtime optimization changed ordered completion outputs')
            entry={'panel':panel,'variant':model['variant'],'profileBytes':model['profileBytes'],
                   'modelSHA256':model['modelSHA256'],'indexSHA256':model['indexSHA256'],
                   'completionOutputMismatches':completion_mismatches,'quality':quality,'comparison':paired}
            report['runs'].append(entry)
            (args.output/'results.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
            print(json.dumps({'panel':panel,'variant':model['variant'],'profileBytes':model['profileBytes'],
                  'falseChanges':[q['autocorrect']['false_change_rate'] for q in quality.values()],
                  'recovery':[q['autocorrect']['corrected_to_expected'] for q in quality.values()]}),flush=True)
    print(json.dumps({'stage':'quality-complete','comparisons':len(report['runs'])}),flush=True)


if __name__=='__main__':main()
