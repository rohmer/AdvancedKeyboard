#!/usr/bin/env python3
"""Create a confirmation panel disjoint from every previously exposed document."""
from __future__ import annotations
import argparse
import collections
import json
from pathlib import Path

import benchmark as bench

DEFAULT_SEED='lvgl-fr-FR-radix-optimization-2026-11-25-confirmation-v1'


def document_ids(path):
    with path.open(encoding='utf-8') as stream:
        return {line.rstrip('\r\n').split('\t',1)[1] for line in stream if line.startswith('#document\t')}


def prepare(prepared,output,seed=DEFAULT_SEED,completion_words=2500,correction_words=1000):
    prepared,output=prepared.resolve(),output.resolve()
    if output==prepared or output.is_relative_to(prepared) or prepared.is_relative_to(output):
        raise ValueError('Confirmation output must be outside the immutable prepared corpus')
    if output.exists() and any(output.iterdir()):
        raise ValueError('Confirmation output must be new or empty')
    exposed=document_ids(prepared/'test.tsv')
    validation=document_ids(prepared/'validation.tsv')
    output.mkdir(parents=True,exist_ok=True)
    counts=collections.Counter();documents=set()
    with (prepared/'test.jsonl').open(encoding='utf-8') as source,(output/'confirmation.jsonl').open('w',encoding='utf-8',newline='\n') as target:
        for raw in source:
            record=json.loads(raw);counts['totalTestSentences']+=1
            if record['document'] in exposed:
                counts['excludedExposedSentences']+=1
                continue
            if record['document'] in validation:
                raise ValueError('Validation/test document overlap')
            target.write(raw);documents.add(record['document']);counts['remainingSentences']+=1
    queries=bench.make_queries(output/'confirmation.jsonl',output/'confirmation.tsv',completion_words,correction_words,seed)
    selected=document_ids(output/'confirmation.tsv')
    if selected & (exposed|validation):raise ValueError('Confirmation document leakage')
    manifest={'version':1,'sourceCorpusManifestSHA256':bench.digest_file(prepared/'manifest.json'),
        'oldTestQueriesSHA256':bench.digest_file(prepared/'test.tsv'),
        'validationQueriesSHA256':bench.digest_file(prepared/'validation.tsv'),
        'oldExposedDocumentCount':len(exposed),'confirmationAvailableDocumentCount':len(documents),
        'confirmationPanelDocumentCount':len(selected),'counts':dict(counts),'seed':seed,
        'confirmationQueries':queries,'confirmationSentencesSHA256':bench.digest_file(output/'confirmation.jsonl'),
        'methodology':'Old test is exposed regression only. Confirmation excludes every document mentioned anywhere in old test.tsv. Those remaining original held-out sentences were globally purged from all prepared training in the previous task; no retraining or new split required. No confirmation outcomes evaluated before runtime/policy freeze. Exact dedup only; no semantic independence claim.'}
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    return manifest


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepared',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--seed',default=DEFAULT_SEED)
    args=parser.parse_args()
    print(json.dumps(prepare(args.prepared,args.output,args.seed)))


if __name__=='__main__':main()
