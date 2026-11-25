#!/usr/bin/env python3
"""Publish frozen-runtime French lookup/correction measurements and provenance."""
from __future__ import annotations
import argparse
import json
from pathlib import Path

import benchmark as bench

LABELS={'optimized-radix':'Radix counts','optimized-smoothed':'Radix MKN','baseline':'Original compact v3'}
KINDS=('p0','p1','p2','p3','clean','delete')


def read(path):return json.loads(path.read_text(encoding='utf-8'))


def percent(value):return f'{100*value:.2f}'


def interval(value):
    low,high=value['delta95CI']
    return f'{100*value["delta"]:+.3f} [{100*low:+.3f}, {100*high:+.3f}]'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifacts',type=Path,required=True)
    parser.add_argument('--generation',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();base=args.artifacts
    protocol=read(base/'protocol.json');corpus=read(base/'corpus-verification.json')
    prepared_proof=read(base/'prepared-verification.json')
    panels=read(base/'panels/manifest.json');quality=read(base/'quality/results.json')
    runtime=read(base/'runtime-validation/results.json');generation=read(args.generation)
    models=read(base/'baseline/models.json');sources=read(base/'candidate/sources.json')
    original=read(base/'baseline/prepared.json')
    for model in models:
        if any(bench.digest_file(Path(model[key]))!=model[key+'SHA256'] for key in ('model','index')):
            raise ValueError('Current model/index bytes differ from their frozen hashes')
    if not corpus['unchanged'] or not all(item['matchesPrepared'] for item in corpus['originalSources']):
        raise ValueError('Original corpus is not verified unchanged')
    if quality['corpusManifestSHA256']!=panels['sourceCorpusManifestSHA256']:
        raise ValueError('Quality evaluation corpus differs')
    if not prepared_proof['unchanged'] or prepared_proof['manifestSHA256']!=quality['corpusManifestSHA256']:
        raise ValueError('Prepared training bytes were not verified unchanged')
    if generation['corpusManifestSHA256']!=quality['corpusManifestSHA256']:
        raise ValueError('Generation rerun corpus differs')
    expected_generation={'scope':'full prepared training split','workers':4,'trainingMemoryMiB':512,
        'hardMemoryMiB':2048,'trainingBytes':sum(s.get('trainingBytes',0) for s in original['sources'])}
    if any(generation.get(key)!=value for key,value in expected_generation.items()):
        raise ValueError('Full generation resource/input protocol differs')
    if any(run['executableSHA256']!=protocol['generationExecutableSHA256'] for run in generation['runs']):
        raise ValueError('Generation rerun does not use the final frozen executable')
    model_sha=bench.digest_file(base/'baseline/models.json')
    if quality['modelsManifestSHA256']!=model_sha or runtime['modelsManifestSHA256']!=model_sha:
        raise ValueError('Fixed model manifest differs')
    if len(runtime['runs'])!=144 or runtime['repetitions']!=8 or len(runtime['summaries'])!=18:
        raise ValueError('Expected eight repetitions of three runtimes on six models')
    if len(quality['runs'])!=18:raise ValueError('Expected all three quality panels and six models')
    expected_quality={(panel,m['variant'],m['profileBytes']) for panel in protocol['panels'] for m in models}
    if {(r['panel'],r['variant'],r['profileBytes']) for r in quality['runs']}!=expected_quality:
        raise ValueError('Quality panel/model combinations are incomplete or duplicated')
    for run in runtime['runs']:
        if run['measurement']['pinned_logical_cpu']!=runtime['pinnedLogicalCPU'] or run['measurement']['warmup_passes']!=1:
            raise ValueError('Timing affinity/warm-up protocol differs')
        if any(kind['cpp_allocation_calls'] for kind in run['measurement']['by_kind']):
            raise ValueError('Observed a C++ allocation during a native query')
    for entry in quality['runs']:
        expected=protocol['panels'][entry['panel']]['sha256']
        if quality['panelSHA256'][entry['panel']]!=expected or any(q['queriesSHA256']!=expected for q in entry['quality'].values()):
            raise ValueError('Quality query panel changed')
        if entry['completionOutputMismatches']!=0:raise ValueError('Ordered completion outputs changed')
    lookup={(s['variant'],s['profileBytes'],s['runtime']):s for s in runtime['summaries']}
    for model in models:
        selected=[lookup[model['variant'],model['profileBytes'],label] for label in ('baseline','speed-only','final')]
        if len({s['completionFingerprint'] for s in selected})!=1 or selected[0]['correctionFingerprint']!=selected[1]['correctionFingerprint']:
            raise ValueError('Speed-only output parity failed')
    by_model={(m['variant'],m['profileBytes']):m for m in models}
    if len(generation['runs'])!=4:raise ValueError('Expected cold/warm counts and MKN reruns')
    if {(r['variant'],r['temperature']) for r in generation['runs']}!={(v,t) for v in ('optimized-radix','optimized-smoothed') for t in ('cold','warm')}:
        raise ValueError('Generation cold/warm variant combinations differ')
    for run in generation['runs']:
        for size,profile in ((262144,run),(16384,run['batchProfile'])):
            model=by_model[run['variant'],size]
            if any(profile[key]!=model[key] for key in ('modelSHA256','indexSHA256')):
                raise ValueError('Full generation rerun did not preserve model/index bytes')

    lines=['# French radix lookup and correction optimization','',
        'The same full-corpus model/index bytes are compared before and after the runtime changes. '
        'Radix lookups are faster in this Windows experiment; the stricter correction policy suppresses '
        'wrong changes and also loses some useful corrections. Results below keep those effects separate.','',
        '## Corpus, frozen panels and limits','',
        f"- Original text input: {sum(s['bytes'] for s in original['sources'] if 'trainingBytes' in s):,} bytes; prepared training: {sum(s.get('trainingBytes',0) for s in original['sources']):,} bytes.",
        f"- All {len(corpus['originalSources'])} original TXT/crawl inputs were rehashed after lookup measurements: identical inventory, sizes and SHA-256 hashes; no corpus writes.",
        '- Validation was the only panel used to select policy thresholds. The previous published test is explicitly an exposed regression panel.',
        f"- Fresh confirmation excludes all {panels['oldExposedDocumentCount']} document headers in that exposed test, leaving {panels['confirmationAvailableDocumentCount']:,} unused held-out documents with retained sentences.",
        f"- Frozen confirmation: {panels['confirmationQueries']['completionWords']:,} words, {panels['confirmationQueries']['completionQueries']:,} incomplete-prefix queries, 1,000 clean controls and 1,000 one-scalar-deletion typos; 119 completion documents and 109 correction documents.",
        '- Confirmation was opened only after source/policy freeze and accepted validation results. No subsequent threshold tuning used it.',
        '', 'All held-out sentences had already been purged from the prepared training files. '
        'The Wikipedia TXT lost article boundaries; this is a crawl-document holdout, not a Wikipedia article split. '
        'Exact normalized deduplication does not establish semantic independence. The new panel comes from the '
        'same crawl, and synthetic deletion errors do not cover natural user typing errors.','',
        '## Repeated native lookup measurements','',
        f"Eight native processes per runtime/model, pinned to logical CPU {runtime['pinnedLogicalCPU']}; runtime order rotates and reverses. "
        'Every process preloads/normalizes the same queries, warms one complete pass and measures the next pass. '
        'Other builds and heavy I/O were held idle. Entries are medians of the eight per-run p50/p95 measurements, '
        'in microseconds; per-run values and min/max ranges are retained in JSON. These are desktop prediction '
        'calls, not end-to-end LVGL or ESP32 hardware latency.','',
        '| Artifact | Nominal profile | Prefix scalars | Baseline p50 / p95 µs | Speed-only p50 / p95 µs | Final p50 / p95 µs | Final p95 change % |',
        '|---|---:|---:|---:|---:|---:|---:|']
    for model in models:
        for kind in range(4):
            values=[lookup[model['variant'],model['profileBytes'],label]['byKind'][kind] for label in ('baseline','speed-only','final')]
            text=[f'{v["p50MedianUs"]:.2f} / {v["p95MedianUs"]:.2f}' for v in values]
            change=100*(values[2]['p95MedianUs']/values[0]['p95MedianUs']-1)
            lines.append(f'| {LABELS[model["variant"]]} | {model["profileBytes"]//1024} KiB | {kind} | '+ ' | '.join(text)+f' | {change:+.2f} |')
    lines+=['','Speed-only preserves every ordered suggested/corrected word on these panels. Fingerprints '
        'cover ordered words, not suggestion masses or correction confidence; numeric/cache metadata '
        'is checked separately by native fixtures. Final preserves every ordered '
        'completion word while applying the fixed correction gates. The v3 rows show a small measured '
        'slowdown and are retained as compatibility evidence; acceleration is not claimed for every format. '
        'Different artifact rows retain different vocabularies and storage costs, so compare runtimes within a row.','',
        '| Artifact | Profile | Input kind | Baseline p50 / p95 µs | Speed-only p50 / p95 µs | Final p50 / p95 µs |',
        '|---|---:|---|---:|---:|---:|']
    for model in models:
        for kind in (4,5):
            values=[lookup[model['variant'],model['profileBytes'],label]['byKind'][kind] for label in ('baseline','speed-only','final')]
            text=[f'{v["p50MedianUs"]:.2f} / {v["p95MedianUs"]:.2f}' for v in values]
            lines.append(f'| {LABELS[model["variant"]]} | {model["profileBytes"]//1024} KiB | {KINDS[kind]} | '+' | '.join(text)+' |')
    lines+=['','Known-word retention often exits early on clean inputs. Its latency improvement is partly a '
        'policy effect; the speed-only column isolates lookup changes.','',
        '## Runtime memory and traversal','',
        '| Runtime | Model object bytes | Prefix scratch bytes | Correction scratch bytes | Cursor object bytes | Max probe private commit MiB |',
        '|---|---:|---:|---:|---:|---:|']
    for label in ('baseline','speed-only','final'):
        values=[s for s in runtime['summaries'] if s['runtime']==label];s=values[0]
        lines.append(f'| {label} | {s["modelObjectBytes"]:,} | {s["prefixScratchBytes"]:,} | {s["correctionScratchBytes"]:,} | {s["cursorObjectBytes"]:,} | {max(v["nativeProcessPeakMemoryBytes"] for v in values)/1048576:.2f} |')
    lines+=['','No C++ allocation-operator call was observed inside any measured query. Direct C allocation '
        'functions are not intercepted. Native process private commit includes the executable, model/index, '
        'preloaded queries and probe; object sizes are separate desktop-ABI subtotals. Cursor reuse adds '
        'caller-local cursor state, and the policy adds scalar scoring state. The cursor size is not a '
        'measurement of maximum stack usage. No expanded trie or persistent heap cache is introduced.','',
        'The final ESP32 compile/link checks report an 80-byte target cursor and 11,724 bytes for '
        '`Model + PrefixScratch + AutocorrectScratch`. These are target ABI object sizes, not hardware '
        'peak memory or stack measurements. The linked 886,592-byte test firmware includes a 16-byte '
        'introspection fixture; no hardware execution or latency measurement was performed.','',
        '| Radix artifact | Profile | Next-word checkpoints before / after | Physical record decodes before / after |',
        '|---|---:|---:|---:|']
    for model in models:
        if model['variant']=='baseline':continue
        filename=f'{model["variant"]}-{model["profileBytes"]}.json'
        before=read(base/'model-trace/results'/filename)['by_kind'][0]
        after=read(base/'model-trace-speed1/results'/filename)['by_kind'][0]
        lines.append(f'| {LABELS[model["variant"]]} | {model["profileBytes"]//1024} KiB | {before["operations"][2]:,} / {after["operations"][2]:,} | {before["operations"][5]:,} / {after["operations"][5]:,} |')
    lines+=['','Counters cover the 2,500 validation next-word queries, using separately instrumented frozen '
        'sources with identical output fingerprints. Instrumented timings are excluded.','',
        '## Correction quality','',
        'Policy: retain exact known words with finite score; require exactly one Unicode edit or adjacent '
        'transposition; require a 10% heuristic score-odds lead over the original spelling and runner-up candidate. '
        'The existing confidence threshold remains 0.3. Percentages below use the same 1,000 clean controls '
        'and 1,000 deletion typos within each panel. Wrong replacements are changed typos that do not '
        'match the expected word. All ordered completion outputs are unchanged.','',
        '| Panel | Artifact | Profile | Clean false changes % before → after | Wrong typo replacements % before → after | Successful recovery % before → after |',
        '|---|---|---:|---:|---:|---:|']
    for entry in quality['runs']:
        metrics=entry['comparison']['metrics'];cells=[]
        for key in ('cleanFalseChanges','deletionWrongChanges','deletionRecovery'):
            value=metrics[key];cells.append(f'{percent(value["baseline"])} → {percent(value["candidate"])}')
        lines.append(f'| {entry["panel"]} | {LABELS[entry["variant"]]} | {entry["profileBytes"]//1024} KiB | '+' | '.join(cells)+' |')
    lines+=['','Fresh confirmation paired differences are candidate minus baseline in percentage points. '
        'Intervals use 2,000 resamples of whole content-hashed documents, preserving query pairing; '
        '109 documents contribute correction queries. Lower false/wrong changes are better; higher '
        'recovery is better. These are descriptive intervals without a multiple-comparison adjustment.','',
        '| Artifact | Profile | Clean false-change delta pp [95% CI] | Wrong replacement delta pp [95% CI] | Recovery delta pp [95% CI] |',
        '|---|---:|---:|---:|---:|']
    for entry in quality['runs']:
        if entry['panel']!='confirmation':continue
        metrics=entry['comparison']['metrics']
        lines.append(f'| {LABELS[entry["variant"]]} | {entry["profileBytes"]//1024} KiB | '+' | '.join(interval(metrics[key]) for key in ('cleanFalseChanges','deletionWrongChanges','deletionRecovery'))+' |')
    lines+=['','The 256 KiB radix-count model has 32.3% fewer clean false changes and 27.3% fewer '
        'wrong typo replacements on confirmation, with recovery falling 59.5% → 57.9%. The 16 KiB '
        'radix profiles preserve deletion recovery on this panel. MKN clean-false-change intervals '
        'touch zero. These observations do not establish a universally optimal policy.','',
        '## Full generation rerun','',
        'After all platform builds finished, the full prepared training input was processed again, with '
        'four workers, a 512 MiB requested training budget and a 2 GiB native job private-commit ceiling. '
        'Cold means an empty dedicated training cache with caching enabled; OS file caches were not '
        'flushed. Each invocation emits both profiles. Every NGTR/NGIX SHA-256 matches the fixed models '
        'used above. Training/encoding algorithms were unchanged; differences from historical timing '
        'are not attributed to lookup optimizations.','',
        'A read-only MKN-cold process sample found generator and parent affinity `0xFFFFFF` '
        '(all 24 logical CPUs), Normal priority, and 603.25 CPU seconds over 207.11 wall seconds. '
        'The benchmark job configures memory/kill-on-close limits, without CPU-rate or affinity limits; '
        'external ancestor job limits were not introspected. This rules out observed single-CPU '
        'inheritance from the microbenchmark. Generation timings are unpinned, single observations '
        'and are not a paired historical acceleration/regression experiment.','',
        '| Variant | Cache | Artifact-ready seconds | Peak native private MiB | 256 KiB profile combined bytes | 16 KiB profile combined bytes |',
        '|---|---|---:|---:|---:|---:|']
    for run in generation['runs']:
        lines.append(f'| {LABELS[run["variant"]]} | {run["temperature"]} | {run["artifactReadySeconds"]:.3f} | {run["peakMemoryBytes"]/1048576:.2f} | {run["combinedModelBytes"]:,} | {run["batchProfile"]["bytes"]:,} |')
    lines+=['','## Provenance','',
        f'- Corpus manifest: `{quality["corpusManifestSHA256"]}`.',
        f'- Confirmation query panel: `{panels["confirmationQueries"]["sha256"]}`.',
        f'- Candidate runtime/managed source manifest: `{bench.digest_file(base/"candidate/sources.json")}`.',
        f'- Candidate PredictionModel.cpp: `{sources["PredictionModel.cpp"]}`.',
        f'- Candidate Utf8Fold.cpp: `{sources["Utf8Fold.cpp"]}`.',
        f'- Timing protocol manifest: `{bench.digest_file(base/"protocol.json")}`.',
        f'- Final generator executable: `{generation["runs"][0]["executableSHA256"]}`.','',
        '| Runtime | Microprofile executable SHA-256 |', '|---|---|']
    for label in ('baseline','speed-only','final'):
        values={r['executableSHA256'] for r in runtime['runs'] if r['runtime']==label}
        if len(values)!=1:raise ValueError('Mixed microprofile snapshots')
        lines.append(f'| {label} | `{next(iter(values))}` |')
    lines+=['', 'Host: Intel Core i9-12900K, 16 cores/24 logical processors; Windows 11 Pro '
        '10.0.26300; 68,494,786,560 bytes physical RAM; MSVC 14.51.36231 x64 Release; CMake 4.5.1; Python 3.11.9.','',
        'Retained artifacts under `build/radix-optimization`: source/binary/model snapshots and hashes, '
        'corpus verification, frozen panel manifest, every command and repeated latency sample, native '
        'outcome fingerprints, paired comparisons, separate traversal counters and full generation logs. '
        'See the adjacent README for the tools and measurement protocol.','']
    args.output.write_text('\n'.join(lines),encoding='utf-8');print(args.output)


if __name__=='__main__':main()
