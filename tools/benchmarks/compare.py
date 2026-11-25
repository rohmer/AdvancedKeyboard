#!/usr/bin/env python3
"""Paired, document-clustered comparison of native prediction outcomes."""
import argparse
import collections
import csv
import json
from pathlib import Path
import random

import benchmark as bench


def load(path):
    data=json.loads(path.read_text(encoding='utf-8'))
    outcomes=path.with_name(path.stem+'-outcomes.tsv')
    if data['outcomesSHA256']!=bench.digest_file(outcomes): raise ValueError('Outcomes hash mismatch')
    with outcomes.open(encoding='utf-8',newline='') as stream:
        rows=list(csv.DictReader(stream,delimiter='\t'))
    return data,rows


def metric_pairs(left,right,kinds,field,known=None):
    samples=[]
    def value(row):
        return float(row['changed'])-float(row['corrected']) if field=='wrongChange' else float(row[field])
    for a,b in zip(left,right):
        if a['kind'] not in kinds: continue
        if known=='common' and not (a['known']=='1' and b['known']=='1'): continue
        samples.append((a['document'],value(a),value(b)))
    return samples


def estimate(samples,repeats,seed):
    if not samples:return {'queries':0,'documents':0,'baseline':None,'candidate':None,'delta':None,'delta95CI':None}
    grouped=collections.defaultdict(lambda:[0.,0.,0])
    for document,a,b in samples:
        group=grouped[document];group[0]+=a;group[1]+=b;group[2]+=1
    base=sum(a for _,a,_ in samples)/len(samples)
    candidate=sum(b for _,_,b in samples)/len(samples)
    groups=list(grouped.values())
    result={'queries':len(samples),'documents':len(groups),'baseline':base,'candidate':candidate,'delta':candidate-base,'delta95CI':None}
    if len(groups)<10 or 'ungrouped' in grouped:
        result['confidenceLimitation']='Too few real document groups for cluster confidence intervals'
        return result
    rng=random.Random(seed)
    deltas=[]
    for _ in range(repeats):
        denominator=delta=0
        for _ in groups:
            a,b,count=groups[rng.randrange(len(groups))]
            delta+=b-a;denominator+=count
        deltas.append(delta/denominator)
    deltas.sort()
    result['delta95CI']=[deltas[int((len(deltas)-1)*.025)],deltas[int((len(deltas)-1)*.975)]]
    return result


def compare(baseline,candidate,repeats=2000):
    a,left=load(baseline);b,right=load(candidate)
    if a['queriesSHA256']!=b['queriesSHA256'] or len(left)!=len(right):raise ValueError('Queries differ')
    for row_a,row_b in zip(left,right):
        if any(row_a[key]!=row_b[key] for key in ('row','kind','document')):raise ValueError('Outcome rows do not align')
    metrics={}
    for label,kinds,field,direction in (
        ('completionTop1',{'p0','p1','p2','p3'},'top1','higher'),
        ('completionTop3',{'p0','p1','p2','p3'},'top3','higher'),
        ('completionMRR',{'p0','p1','p2','p3'},'reciprocal','higher'),
        ('nextWordTop3',{'p0'},'top3','higher'),
        ('deletionRecovery',{'delete'},'corrected','higher'),
        ('deletionWrongChanges',{'delete'},'wrongChange','lower'),
        ('cleanFalseChanges',{'clean'},'changed','lower')):
        value=estimate(metric_pairs(left,right,kinds,field),repeats,1729)
        value['betterDirection']=direction
        metrics[label]=value
        if label in ('completionTop3','deletionRecovery','cleanFalseChanges'):
            common=estimate(metric_pairs(left,right,kinds,field,'common'),repeats,1729)
            common['betterDirection']=direction
            metrics[label+'CommonVocabulary']=common
    fingerprints=(sum(row_a.get('outputFingerprint')!=row_b.get('outputFingerprint') for row_a,row_b in zip(left,right))
                  if left and 'outputFingerprint' in left[0] and 'outputFingerprint' in right[0] else None)
    return {'baseline':str(baseline),'candidate':str(candidate),'queriesSHA256':a['queriesSHA256'],
            'orderedOutputFingerprintMismatches':fingerprints,
            'bootstrapReplicates':repeats,'bootstrapUnit':'content-hashed crawl document',
            'baselineModelAndIndexBytes':a['model_bytes']+a['index_bytes'],
            'candidateModelAndIndexBytes':b['model_bytes']+b['index_bytes'],'metrics':metrics,
            'calibratedNLLLimitation':'NLL is not compared across differing vocabularies or against an uncalibrated legacy priority-fallback model.'}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline',type=Path,required=True);parser.add_argument('--candidate',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True);parser.add_argument('--repeats',type=int,default=2000)
    args=parser.parse_args()
    result=compare(args.baseline,args.candidate,args.repeats)
    args.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({key:result['metrics'][key] for key in ('completionTop3','nextWordTop3','cleanFalseChanges')}))


if __name__=='__main__':main()
