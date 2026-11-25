#!/usr/bin/env python3
"""Publish the measured French corpus tables from retained JSON artifacts."""
import argparse
import hashlib
import html
import json
import math
from pathlib import Path


def require_fields(data,expected,source):
    for field,value in expected.items():
        if data.get(field)!=value:
            raise ValueError(f'{source}: {field} must be {value!r}, got {data.get(field)!r}')


def require_quality(quality,query_sha256,source,probe_sha256=None):
    expected={'queriesSHA256':query_sha256}
    if probe_sha256 is not None:expected['probeSHA256']=probe_sha256
    require_fields(quality,expected,source)
    return quality['model_bytes']+quality['index_bytes']


def table_text(value):
    value=html.escape(str(value),quote=False).replace('\r',' ').replace('\n',' ').replace('|','&#124;')
    for character in ('\\','`','*','_','[',']'):
        value=value.replace(character,'\\'+character)
    return value


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepared',type=Path,required=True)
    parser.add_argument('--baseline',type=Path,required=True)
    parser.add_argument('--compact',type=Path,required=True)
    parser.add_argument('--radix',type=Path,required=True)
    parser.add_argument('--source-manifest',type=Path,required=True)
    parser.add_argument('--equal-storage',type=Path)
    parser.add_argument('--format-only',type=Path)
    parser.add_argument('--comparisons',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    read=lambda path:json.loads(path.read_text(encoding='utf-8'))
    manifest_path=args.prepared/'manifest.json'
    manifest=read(manifest_path)
    corpus_manifest_sha256=hashlib.sha256(manifest_path.read_bytes()).hexdigest()
    query_sha256=hashlib.sha256((args.prepared/'test.tsv').read_bytes()).hexdigest()
    if query_sha256!=manifest['queries']['test']['sha256']:
        raise ValueError('Prepared test queries do not match the corpus manifest')
    input_paths=(args.baseline,args.compact,args.radix)
    collections=[read(path) for path in input_paths]
    expected_fields={'scope':'full prepared training split','evaluationSplit':'test',
                     'corpusManifestSHA256':corpus_manifest_sha256,
                     'trainingBytes':sum(x['trainingBytes'] for x in manifest['sources'] if 'trainingBytes' in x)}
    for path,collection in zip(input_paths,collections):
        require_fields(collection,expected_fields,path)
        for run in collection['runs']:
            qualities=[run['quality']]
            if 'batchProfile' in run:qualities.append(run['batchProfile']['quality'])
            for quality in qualities:require_quality(quality,query_sha256,path)
    runs=[run for collection in collections for run in collection['runs']]
    sources=read(args.source_manifest)
    labels={'baseline':'Original compact counts','optimized-compact':'Optimized compact counts',
            'optimized-radix':'Radix counts','optimized-smoothed':'Radix modified Kneser–Ney'}
    provenance={}
    for run in runs:
        snapshot=(run['executableSHA256'],run['quality']['probeSHA256'])
        if run['variant'] in provenance and provenance[run['variant']]!=snapshot:
            raise ValueError(f"Mixed executable snapshots for {run['variant']}")
        if 'batchProfile' in run and run['batchProfile']['quality']['probeSHA256']!=snapshot[1]:
            raise ValueError(f"Mixed prediction probes for {run['variant']}")
        provenance[run['variant']]=snapshot
    for variant in ('optimized-radix','optimized-smoothed'):
        if variant in provenance and provenance[variant]!=(sources['generatorExecutableSHA256'],sources['probeExecutableSHA256']):
            raise ValueError(f'{variant} does not match the final source manifest executables')
    baseline_profiles={run['profileBytes']:run['quality'] for run in runs if run['variant']=='baseline'}
    baseline_cold=next(run for run in runs if run['variant']=='baseline' and run['temperature']=='cold')
    equal_storage=read(args.equal_storage) if args.equal_storage else None
    if equal_storage is not None:
        require_fields(equal_storage,expected_fields,args.equal_storage)
        require_fields(equal_storage,{'largeBudgetBytes':470297,'smallBudgetBytes':32550},args.equal_storage)
        if len(equal_storage['runs'])!=2 or {run['variant'] for run in equal_storage['runs']}!={'optimized-radix','optimized-smoothed'}:
            raise ValueError('Equal-storage results must contain radix counts and modified Kneser–Ney')
        for size,budget in ((262144,equal_storage['largeBudgetBytes']),(16384,equal_storage['smallBudgetBytes'])):
            if require_quality(baseline_profiles[size],query_sha256,args.baseline)!=budget:
                raise ValueError(f'Equal-storage budget does not match the original {size}-byte profile')
        for run in equal_storage['runs']:
            require_fields(run,{'temperature':'equal-storage-warm',
                               'executableSHA256':sources['generatorExecutableSHA256'],
                               'profileBytes':equal_storage['largeBudgetBytes']},args.equal_storage)
            require_fields(run['batchProfile'],{'budgetBytes':equal_storage['smallBudgetBytes']},args.equal_storage)
            for entry,budget,recorded_bytes in (
                (run,equal_storage['largeBudgetBytes'],run['combinedModelBytes']),
                (run['batchProfile'],equal_storage['smallBudgetBytes'],run['batchProfile']['bytes'])):
                total=require_quality(entry['quality'],query_sha256,args.equal_storage,sources['probeExecutableSHA256'])
                if total!=recorded_bytes or total>budget:
                    raise ValueError(f'{args.equal_storage}: emitted model/index bytes do not match the storage budget')
    format_only=read(args.format_only) if args.format_only else None
    completion_documents=116
    correction_documents=99
    if format_only is not None:
        require_fields(format_only,{'corpusManifestSHA256':corpus_manifest_sha256},args.format_only)
        if len(format_only['profiles'])!=2 or {profile['nominalProfileBytes'] for profile in format_only['profiles']}!={16384,262144}:
            raise ValueError('Format-only results must contain both original model profiles')
        for profile in format_only['profiles']:
            size=profile['nominalProfileBytes']
            original=baseline_profiles[size]
            source_bytes=require_quality(original,query_sha256,args.baseline)
            target_bytes=require_quality(profile['quality'],query_sha256,args.format_only,sources['probeExecutableSHA256'])
            require_fields(profile,{'sourceCombinedBytes':source_bytes,'targetCombinedBytes':target_bytes},args.format_only)
            comparison=profile['comparison']
            require_fields(comparison,{'queriesSHA256':query_sha256,
                                      'baselineModelAndIndexBytes':source_bytes,
                                      'candidateModelAndIndexBytes':target_bytes,
                                      'orderedOutputFingerprintMismatches':0},args.format_only)
            metric=comparison['metrics']['completionTop3']
            if abs(metric['baseline']-original['completion']['top3_all'])>1e-10 or abs(metric['candidate']-profile['quality']['completion']['top3_all'])>1e-10 or abs(metric['delta'])>1e-10:
                raise ValueError(f'{args.format_only}: format-only top-3 metrics do not preserve the original result')
            if abs(original['completion']['vocabulary_coverage']-profile['quality']['completion']['vocabulary_coverage'])>1e-10:
                raise ValueError(f'{args.format_only}: format-only vocabulary coverage differs')
            completion_documents=metric.get('documents',completion_documents)
            correction_documents=comparison['metrics'].get('deletionRecovery',{}).get('documents',correction_documents)
    paired=read(args.comparisons) if args.comparisons else None
    paired_metrics=(('completionTop3','Completion top-3'),('nextWordTop3','Next-word top-3'),
                    ('deletionRecovery','Deletion recovery'),('cleanFalseChanges','Clean false changes'))
    if paired is not None:
        require_fields(paired,{'corpusManifestSHA256':corpus_manifest_sha256,
                              'queriesSHA256':query_sha256},args.comparisons)
        if not paired['comparisons']:
            raise ValueError('Paired comparison input must contain at least one comparison')
        document_counts=set()
        for entry in paired['comparisons']:
            if not isinstance(entry['label'],str):raise ValueError('Comparison labels must be text')
            comparison=entry['comparison']
            require_fields(comparison,{'queriesSHA256':query_sha256,
                                       'bootstrapUnit':'content-hashed crawl document'},args.comparisons)
            if not isinstance(comparison['bootstrapReplicates'],int) or comparison['bootstrapReplicates']<=0:
                raise ValueError('Paired comparison bootstrap replicate count must be positive')
            for name,_ in paired_metrics:
                metric=comparison['metrics'][name]
                if any(not isinstance(metric[key],(int,float)) or not math.isfinite(metric[key]) for key in ('baseline','candidate','delta')):
                    raise ValueError(f'{args.comparisons}: {name} has invalid paired estimates')
                if abs(metric['delta']-(metric['candidate']-metric['baseline']))>1e-10:
                    raise ValueError(f'{args.comparisons}: {name} delta does not match its paired estimates')
                if any(not isinstance(metric[key],int) or metric[key]<=0 for key in ('queries','documents')):
                    raise ValueError(f'{args.comparisons}: {name} must have real query/document counts')
                interval=metric['delta95CI']
                if interval is not None and (not isinstance(interval,list) or len(interval)!=2 or
                    any(not isinstance(value,(int,float)) or not math.isfinite(value) for value in interval) or interval[0]>interval[1]):
                    raise ValueError(f'{args.comparisons}: {name} has an invalid bootstrap interval')
            document_counts.add((comparison['metrics']['completionTop3']['documents'],
                                 comparison['metrics']['deletionRecovery']['documents']))
        if len(document_counts)!=1:
            raise ValueError('Paired comparisons disagree on evaluated completion/correction document counts')
        completion_documents,correction_documents=document_counts.pop()
    lines=['# French corpus benchmark results','',
           'Measured on the complete prepared training split derived from `D:\\Corpus\\fr-FR`. '
           'Development-pilot measurements are excluded from these tables.','',
           '## Corpus and evaluation','',
           f"- Original TXT input: {sum(x['bytes'] for x in manifest['sources'] if 'trainingBytes' in x):,} bytes across three files.",
           f"- Prepared training input: {sum(x['trainingBytes'] for x in manifest['sources'] if 'trainingBytes' in x):,} bytes.",
           f"- Exact training duplicates removed: {manifest['counts']['duplicate_training_sentences']:,}; held-out sentence overlaps purged: {manifest['counts']['purged_heldout_overlaps']:,}.",
           f"- Content-grouped crawl documents: {manifest['counts']['train_documents']:,} training, {manifest['counts']['validation_documents']:,} validation, {manifest['counts']['test_documents']:,} test.",
           f"- Final test: {manifest['queries']['test']['completionWords']:,} words, {manifest['queries']['test']['completionQueries']:,} incomplete-prefix queries, {manifest['queries']['test']['cleanCorrectionQueries']:,} clean correction controls and {manifest['queries']['test']['deletionCorrectionQueries']:,} one-scalar-deletion typos.",
           f'- Evaluated query panel: {completion_documents:,} held-out documents for completion and {correction_documents:,} for correction.',
           '',manifest['wikipediaSplitLimitation'],manifest['nearDuplicateLimitation'],'',
           'The same frozen training, word list and sealed test queries are used for every variant. '
           'Validation pages were used for development; test results were collected after source freeze. '
           'Corpus preparation is a shared preprocessing step and is excluded from generation timing.','',
           '## Generation time and memory','',
           'Four counting workers; optimized training budget 512 MiB; native generator process-tree private-commit ceiling 2 GiB. '
           'Cold means an empty model-training cache with caching enabled. OS file caches were not flushed. '
           'Memory below is the native generator Windows job peak private committed bytes, not RSS. '
           'These are individual measured runs, not claimed universal speedups.','',
           '| Variant | Run | Artifact-ready seconds | Generator peak private MiB | Profiles produced |',
           '|---|---|---:|---:|---|']
    for run in runs:
        profiles='256 + 16 KiB batch' if 'batchProfile' in run else ('16 KiB' if run['profileBytes']==16384 else '256 KiB')
        lines.append(f"| {labels[run['variant']]} | {run['temperature']} | {run['artifactReadySeconds']:.3f} | {run['peakMemoryBytes']/1048576:.2f} | {profiles} |")
    lines+=['',f"The preserved native baseline produced the 256 KiB model and index in {baseline_cold['artifactReadySeconds']:.1f} seconds. "
            'The previously user-reported 4–5-hour workflow was not reproduced by this measured native CLI workload.','',
            'Artifact-ready time includes required index construction. The separate Python index builder used for '
            'compact models is outside the measured native generator job: its memory is unmeasured and is not subject '
            'to the stated 2 GiB ceiling. Legacy compact profiles require '
            'separate invocations; radix exports both sizes from one training pass. '
            'Cached small-profile legacy runs reuse the same count/training cache.','',
            '## Storage, prediction quality and native lookup latency','',
            'Legacy nominal budgets constrain NGTR alone; radix budgets constrain NGTR plus NGIX. '
            'Use the actual combined bytes below when comparing quality. Coverage is vocabulary coverage '
            'over the same eligible completion queries. All-query accuracy includes out-of-vocabulary targets.','',
            '| Variant | Nominal profile | NGTR + NGIX bytes | Top-3 % | Next-word top-3 % | Coverage % | Clean false-change % | Deletion recovery % | Completion p95 µs |',
            '|---|---|---:|---:|---:|---:|---:|---:|---:|']
    for run in runs:
        if run['temperature']=='warm':continue
        entries=[(run['profileBytes'],run['quality'])]
        if 'batchProfile' in run:entries.append((16384,run['batchProfile']['quality']))
        for size,quality in entries:
            complete=quality['completion'];correct=quality['autocorrect']
            lines.append(f"| {labels[run['variant']]} | {size//1024} KiB | {quality['model_bytes']+quality['index_bytes']:,} | {100*complete['top3_all']:.3f} | {100*quality['by_prefix'][0]['top3_all']:.3f} | {100*complete['vocabulary_coverage']:.3f} | {100*correct['false_change_rate']:.3f} | {100*correct['corrected_to_expected']:.3f} | {quality['completion_p95_us']:.2f} |")
    if equal_storage is not None:
        lines+=['','## Equal combined-storage budgets','',
                'The original compact models set the shared NGTR + NGIX limits. Radix counts and modified '
                'Kneser–Ney are generated from the same full-corpus cached training data under those limits. '
                'The models may use fewer bytes than the limit. These rows compare retrained models at '
                'the same maximum combined storage; their vocabularies can differ.','',
                '| Variant | Combined budget bytes | NGTR + NGIX bytes | Top-3 % | Top-3 delta pp | Next-word top-3 % | Coverage % | Clean false-change % | Deletion recovery % |',
                '|---|---:|---:|---:|---:|---:|---:|---:|---:|']
        for size,budget in ((262144,equal_storage['largeBudgetBytes']),(16384,equal_storage['smallBudgetBytes'])):
            original=baseline_profiles[size]
            entries=[('baseline',original)]
            entries.extend((run['variant'],run['quality'] if size==262144 else run['batchProfile']['quality']) for run in equal_storage['runs'])
            for variant,quality in entries:
                complete=quality['completion'];correct=quality['autocorrect']
                delta=100*(complete['top3_all']-original['completion']['top3_all'])
                lines.append(f"| {labels[variant]} | {budget:,} | {quality['model_bytes']+quality['index_bytes']:,} | {100*complete['top3_all']:.3f} | {delta:+.3f} | {100*quality['by_prefix'][0]['top3_all']:.3f} | {100*complete['vocabulary_coverage']:.3f} | {100*correct['false_change_rate']:.3f} | {100*correct['corrected_to_expected']:.3f} |")
    if format_only is not None:
        lines+=['','## Format-only conversion','',
                'The original compact models are converted to radix without retraining. The retained test '
                'panel has zero ordered suggestion/correction output fingerprint mismatches and unchanged '
                'top-3 accuracy and vocabulary coverage. This measures representation savings and observed '
                'test-panel parity; it does not establish behavior for every possible input.','',
                '| Original nominal profile | Original NGTR + NGIX bytes | Radix NGTR + NGIX bytes | Storage reduction % | Output fingerprint mismatches | Top-3 delta pp |',
                '|---|---:|---:|---:|---:|---:|']
        for profile in sorted(format_only['profiles'],key=lambda item:item['nominalProfileBytes'],reverse=True):
            reduction=100*(1-profile['targetCombinedBytes']/profile['sourceCombinedBytes'])
            comparison=profile['comparison']
            lines.append(f"| {profile['nominalProfileBytes']//1024} KiB | {profile['sourceCombinedBytes']:,} | {profile['targetCombinedBytes']:,} | {reduction:.3f} | {comparison['orderedOutputFingerprintMismatches']:,} | {100*comparison['metrics']['completionTop3']['delta']:+.3f} |")
    lines+=['','## Native runtime memory and latency','',
            'Workspace bytes are the desktop ABI subtotal `sizeof(Model) + sizeof(PrefixScratch) + '
            'sizeof(AutocorrectScratch)`. They exclude serialized model/index bytes, probe/query overhead '
            'and temporary call-stack locals. Lookup latency covers native prediction/correction calls; '
            'query parsing and French normalization occur outside those timings. Nominal-profile rows '
            'reuse the quality measurements collected after cold generation or cached small-profile export.','',
            '| Cohort | Variant | Profile or combined budget | Workspace bytes | Logical nodes | Physical nodes | Completion p50 µs | Completion p95 µs | Correction p50 µs | Correction p95 µs |',
            '|---|---|---|---:|---:|---:|---:|---:|---:|---:|']
    runtime_rows=[]
    for run in runs:
        if run['temperature']=='warm':continue
        runtime_rows.append(('Nominal',labels[run['variant']],f"{run['profileBytes']//1024} KiB",run['quality']))
        if 'batchProfile' in run:
            runtime_rows.append(('Nominal',labels[run['variant']],'16 KiB',run['batchProfile']['quality']))
    if equal_storage is not None:
        for run in equal_storage['runs']:
            runtime_rows.append(('Equal storage',labels[run['variant']],f"{equal_storage['largeBudgetBytes']:,} bytes",run['quality']))
            runtime_rows.append(('Equal storage',labels[run['variant']],f"{equal_storage['smallBudgetBytes']:,} bytes",run['batchProfile']['quality']))
    if format_only is not None:
        for profile in sorted(format_only['profiles'],key=lambda item:item['nominalProfileBytes'],reverse=True):
            runtime_rows.append(('Format only','Converted radix counts',f"{profile['nominalProfileBytes']//1024} KiB source",profile['quality']))
    for cohort,variant,profile,quality in runtime_rows:
        lines.append(f"| {cohort} | {variant} | {profile} | {quality['workspace_bytes']:,} | {quality['logical_nodes']:,} | {quality['physical_nodes']:,} | {quality['completion_p50_us']:.2f} | {quality['completion_p95_us']:.2f} | {quality['autocorrect_p50_us']:.2f} | {quality['autocorrect_p95_us']:.2f} |")
    if paired is not None:
        lines+=['','## Paired document-bootstrap comparisons','',
                'Changes are candidate minus baseline in percentage points over the identical test queries. '
                'The 95% intervals resample whole content-hashed crawl documents, keeping each document\'s '
                'queries paired. Lower clean false-change rates are better; higher values are better for '
                'the other three metrics. An unavailable interval is shown explicitly.','',
                '| Comparison | Metric | Queries | Documents | Delta pp | 95% CI pp |',
                '|---|---|---:|---:|---:|---|']
        for entry in paired['comparisons']:
            for name,label in paired_metrics:
                metric=entry['comparison']['metrics'][name]
                interval=metric['delta95CI']
                interval_text='unavailable' if interval is None else f'[{100*interval[0]:+.3f}, {100*interval[1]:+.3f}]'
                lines.append(f"| {table_text(entry['label'])} | {label} | {metric['queries']:,} | {metric['documents']:,} | {100*metric['delta']:+.3f} | {interval_text} |")
    lines+=['','The full JSON retains top-1, MRR, in-vocabulary metrics and calibrated log loss where available. '
            'Log loss is not compared across '
            'different vocabulary/OOV mappings; legacy priority fallback has no calibrated probability model. '
            'Desktop timings do not establish ESP32 hardware latency.','',
            '## Reproducibility','',
            f"- Corpus manifest SHA-256: `{corpus_manifest_sha256}`.",
            f"- Test query SHA-256: `{query_sha256}`.",
            f"- Final generator executable SHA-256: `{sources['generatorExecutableSHA256']}`.",
            f"- Final prediction probe SHA-256: `{sources['probeExecutableSHA256']}`.",
            f"- Split seed: `{manifest['seed']}`; query protocol: `{manifest.get('queryProtocol','unknown')}`.",
            '',
            'The optimized compact measurements use the preserved optimized compact snapshot. '
            'Subsequent source-freeze guard fixes did not affect its counting path. '
            'The radix measurements use the final generator and prediction probe recorded in the source manifest.','',
            '| Variant | Generator SHA-256 | Prediction probe SHA-256 |',
            '|---|---|---|']
    for variant,(generator_sha256,probe_sha256) in provenance.items():
        lines.append(f'| {labels[variant]} | `{generator_sha256}` | `{probe_sha256}` |')
    environment={key.casefold():value for key,value in sources.get('environment',{}).items()}
    environment_fields=[]
    for key,label in (('cpu','CPU'),('os','OS'),('ram','RAM'),('compiler','Compiler'),('cmake','CMake'),('python','Python'),('buildmode','Build mode')):
        if key in environment:environment_fields.append(f'{label}: {table_text(environment[key])}')
    if environment_fields:lines+=['','Host and build environment: '+'; '.join(environment_fields)+'.']
    lines+=['',
            'Retained artifacts: corpus/input hashes, derived training hashes, source manifests, every command '
            'line, generation phase timestamps, model/index hashes, native per-query outcome fingerprints '
            'and document-clustered paired comparisons. See the adjacent README for rerunning the harness.','']
    args.output.write_text('\n'.join(lines),encoding='utf-8')
    print(args.output)


if __name__=='__main__':main()
