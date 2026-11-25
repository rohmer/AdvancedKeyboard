#!/usr/bin/env python3
"""Reproducible corpus preparation and native language-model measurements.

The corpus is read-only. All derived corpora, caches, queries and measurements
belong in a new, explicit output directory. No external Python packages required.
"""
from __future__ import annotations
import argparse
import collections
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import sqlite3
import struct
import subprocess
import sys
import time
import unicodedata

NORMALIZATION = 'NFC-lower-curly-apostrophe-collapse-whitespace-v1'


def digest_file(path: Path) -> str:
    value = hashlib.sha256()
    with path.open('rb') as stream:
        while block := stream.read(4 * 1024 * 1024): value.update(block)
    return value.hexdigest()


def normalized_sentence(text: str) -> str:
    return ' '.join(unicodedata.normalize('NFC', text).lower().replace('\u2019', "'").split())


def sentence_digest(text: str) -> bytes:
    return hashlib.sha256(normalized_sentence(text).encode('utf-8')).digest()


def split_for_document(content: str, seed: str) -> str:
    # Identical bodies receive the same split even when mirrored under another URL.
    bucket = int.from_bytes(hashlib.sha256((seed + '\0' + content).encode('utf-8')).digest()[:8], 'big') % 100
    return 'test' if bucket < 10 else 'validation' if bucket < 20 else 'train'


def crawl_records(path: Path):
    """Read C# CrawlCheckpoint's LE-length-prefixed JSON without altering it."""
    with path.open('rb') as stream:
        while header := stream.read(4):
            if len(header) != 4: raise ValueError(f'Incomplete crawl record header: {path}')
            size, = struct.unpack('<i', header)
            if not 0 < size <= 64 * 1024 * 1024: raise ValueError(f'Invalid crawl record length {size}: {path}')
            data = stream.read(size)
            if len(data) != size: raise ValueError(f'Incomplete crawl record: {path}')
            record = json.loads(data)
            if 'Version' in record and 'Start' in record: continue
            page = record.get('Page') or {}
            sentences = page.get('Sentences') or []
            if record.get('Url') and isinstance(sentences, list) and all(isinstance(s, str) for s in sentences):
                yield record['Url'], sentences


def contextual_tokens(text: str, preserve_case: bool = False):
    """French tokens with the trainer's dot/newline sentence resets."""
    text = unicodedata.normalize('NFC', text).replace('\u2019', "'")
    if not preserve_case: text = text.lower()
    result, current = [], []
    previous = older = ''
    def finish():
        nonlocal previous, older
        if current:
            word = ''.join(current)
            result.append((word, previous, older))
            older, previous = previous, word
            current.clear()
    for index, scalar in enumerate(text):
        category = unicodedata.category(scalar)
        word = category[0] in 'LN'
        continuation = category[0] == 'M' and current
        apostrophe = scalar == "'" and current and index + 1 < len(text) and unicodedata.category(text[index + 1])[0] in 'LN'
        if word or continuation or apostrophe: current.append(scalar)
        else:
            finish()
            if scalar in '.\r\n': previous = older = ''
    finish()
    return result


def tokens(text: str):
    return [word for word, _, _ in contextual_tokens(text)]


def prepare(corpus: Path, output: Path, seed: str, completion_words: int, correction_words: int,
            subset_bytes: int = 100 * 1024 * 1024):
    if output.exists() and any(output.iterdir()): raise ValueError('Preparation output must be new or empty')
    output.mkdir(parents=True, exist_ok=True)
    corpus = corpus.resolve()
    source_files = sorted(p for p in corpus.rglob('*') if p.is_file() and p.suffix in ('.txt', '.crawl'))
    txt_files = [p for p in source_files if p.suffix == '.txt']
    crawl_files = [p for p in source_files if p.suffix == '.crawl']
    if not txt_files or not crawl_files: raise ValueError('Expected TXT corpus plus crawl sidecars with page boundaries')
    database = sqlite3.connect(output / 'preparation.sqlite')
    database.execute('PRAGMA journal_mode=WAL')
    database.execute('PRAGMA synchronous=NORMAL')
    database.execute('PRAGMA cache_size=-65536')
    database.executescript('''CREATE TABLE documents(body BLOB PRIMARY KEY, split TEXT NOT NULL, source TEXT NOT NULL, url TEXT NOT NULL) WITHOUT ROWID;
        CREATE TABLE heldout(hash BLOB PRIMARY KEY, split TEXT NOT NULL) WITHOUT ROWID;
        CREATE TABLE seen(hash BLOB PRIMARY KEY) WITHOUT ROWID;''')
    stats = collections.Counter()
    heldout_paths = {split: output / (split + '.jsonl') for split in ('validation', 'test')}
    heldout = {split: path.open('w', encoding='utf-8', newline='\n') for split, path in heldout_paths.items()}
    for path in crawl_files:
        for url, sentences in crawl_records(path):
            canonical = '\n'.join(normalized_sentence(s) for s in sentences if normalized_sentence(s))
            if not canonical: continue
            body_hash = hashlib.sha256(canonical.encode('utf-8')).digest()
            split = split_for_document(canonical, seed)
            inserted = database.execute('INSERT OR IGNORE INTO documents VALUES(?,?,?,?)',
                                        (body_hash, split, str(path.relative_to(corpus)), url)).rowcount
            if not inserted: stats['duplicate_documents'] += 1; continue
            stats[split + '_documents'] += 1
            if split != 'train':
                for sentence in sentences:
                    normalized = normalized_sentence(sentence)
                    if not normalized: continue
                    digest = sentence_digest(sentence)
                    # If duplicates cross validation/test documents, validation wins;
                    # the duplicate is never emitted into both evaluation sets.
                    inserted = database.execute('INSERT OR IGNORE INTO heldout VALUES(?,?)', (digest, split)).rowcount
                    if inserted:
                        heldout[split].write(json.dumps({'document': body_hash.hex(), 'url': url, 'text': sentence}, ensure_ascii=False) + '\n')
                        stats[split + '_sentences'] += 1
        database.commit()
    for stream in heldout.values(): stream.close()
    # Held-out sentences are purged globally, including Wikipedia copies. The
    # TXT export discarded article IDs, so no fictitious article split is made.
    training = output / 'train'; training.mkdir()
    subset = output / 'subset-train'; subset.mkdir()
    subset_threshold = min(1.0, subset_bytes / sum(p.stat().st_size for p in txt_files))
    subset_hash_limit = int(subset_threshold * (1 << 64))
    source_manifest = []
    for source_index, path in enumerate(txt_files):
        source_stat = path.stat()
        target = training / f'{source_index:02d}-{path.name}'
        small = subset / target.name
        input_hash = hashlib.sha256()
        input_bytes = output_bytes = input_lines = kept_lines = 0
        started = time.perf_counter()
        with path.open('rb') as original, target.open('wb') as destination, small.open('wb') as sample:
            for raw in original:
                input_hash.update(raw); input_bytes += len(raw); input_lines += 1
                text = raw.decode('utf-8-sig' if input_lines == 1 else 'utf-8', errors='strict').strip()
                if not text: stats['empty_lines'] += 1; continue
                fingerprint = sentence_digest(text)
                if database.execute('SELECT 1 FROM heldout WHERE hash=?', (fingerprint,)).fetchone():
                    stats['purged_heldout_overlaps'] += 1; continue
                if not database.execute('INSERT OR IGNORE INTO seen VALUES(?)', (fingerprint,)).rowcount:
                    stats['duplicate_training_sentences'] += 1; continue
                encoded = text.encode('utf-8') + b'\n'
                destination.write(encoded); output_bytes += len(encoded); kept_lines += 1
                # A deterministic hash sample spans all sources rather than
                # selecting only the front of the large Wikipedia export.
                if int.from_bytes(fingerprint[:8], 'big') < subset_hash_limit:
                    sample.write(encoded)
                if input_lines % 10000 == 0: database.commit()
                if input_lines % 100000 == 0:
                    print(json.dumps({'stage': 'prepare', 'source': str(path), 'inputBytes': input_bytes,
                                      'keptSentences': kept_lines}), flush=True)
        database.commit()
        after_stat = path.stat()
        if after_stat.st_size != input_bytes or after_stat.st_mtime_ns != source_stat.st_mtime_ns:
            raise RuntimeError('Corpus changed during preparation')
        source_manifest.append({'path': str(path), 'bytes': input_bytes, 'sha256': input_hash.hexdigest(),
                                'inputLines': input_lines, 'trainingBytes': output_bytes, 'trainingSentences': kept_lines,
                                'preparedSHA256': digest_file(target), 'seconds': time.perf_counter() - started})
    # Sidecar fingerprints are also retained; these supplied document membership.
    for path in crawl_files:
        source_manifest.append({'path': str(path), 'bytes': path.stat().st_size, 'sha256': digest_file(path), 'role': 'document-metadata'})
    database.close()
    query_manifest = {}
    for split, path in heldout_paths.items():
        query_manifest[split] = make_queries(path, output / (split + '.tsv'), completion_words, correction_words, seed)
    manifest = {'version': 1, 'corpus': str(corpus), 'seed': seed, 'normalization': NORMALIZATION,
        'pythonUnicodeVersion': unicodedata.unidata_version, 'split': 'crawl content-grouped 80/10/10',
        'wikipediaSplitLimitation': 'TXT contains sentence lines with article IDs and boundaries discarded; held-out crawl sentence overlaps are globally purged from Wikipedia training.',
        'nearDuplicateLimitation': 'Exact normalized document/sentence deduplication; no claim of semantic/paraphrase deduplication.',
        'sources': source_manifest, 'counts': dict(stats), 'queries': query_manifest,
        'trainingFiles': [str(p.resolve()) for p in sorted(training.glob('*.txt'))],
        'subsetFiles': [str(p.resolve()) for p in sorted(subset.glob('*.txt'))],
        'subsetBytes': sum(p.stat().st_size for p in subset.glob('*.txt'))}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(json.dumps({'stage': 'prepared', 'trainingBytes': sum(x.get('trainingBytes', 0) for x in source_manifest),
                      'subsetBytes': manifest['subsetBytes'], 'counts': dict(stats)}), flush=True)
    return manifest


def make_queries(source: Path, destination: Path, completion_words: int, correction_words: int, seed: str):
    # Keep the smallest stable sentence hashes, so ordering and pagination do
    # not bias evaluation toward one early page. Only a bounded heap is kept.
    import heapq
    heap = []
    with source.open(encoding='utf-8') as sentences:
        for raw in sentences:
            record = json.loads(raw)
            value = int.from_bytes(hashlib.sha256((seed + record['text']).encode('utf-8')).digest()[:8], 'big')
            item = (-value, record['text'], record['document'])
            if len(heap) < max(completion_words, correction_words) * 2: heapq.heappush(heap, item)
            elif item > heap[0]: heapq.heapreplace(heap, item)
    selected = sorted(heap, reverse=True)
    completion_count = correction_count = query_count = 0
    prefix_counts = [0, 0, 0, 0]
    with destination.open('w', encoding='utf-8', newline='\n') as output:
        output.write('#kind\texpected\tprevious\tolder\tprefix\ttyped\n')
        for _, sentence, document in selected:
            output.write('#document\t'+document+'\n')
            for word, previous, older in contextual_tokens(sentence, preserve_case=True):
                if not 1 <= len(word) <= 32 or len(word.encode('utf-8')) > 128: continue
                if len(previous.encode('utf-8')) > 128 or len(older.encode('utf-8')) > 128: continue
                if completion_count < completion_words:
                    for length in range(4):
                        if length >= len(word): continue # Never reward an already-complete word.
                        prefix = word[:length]
                        output.write('\t'.join((f'p{length}', word, previous, older, prefix, '')) + '\n')
                        query_count += 1; prefix_counts[length] += 1
                    completion_count += 1
                if correction_count < correction_words and len(word) >= 5 and word.isalpha():
                    position = 1 + int.from_bytes(hashlib.sha256(word.encode('utf-8')).digest()[:2], 'big') % (len(word) - 2)
                    typed = word[:position] + word[position + 1:]
                    output.write('\t'.join(('clean', word, previous, older, '', word)) + '\n')
                    output.write('\t'.join(('delete', word, previous, older, '', typed)) + '\n')
                    correction_count += 1
            if completion_count >= completion_words and correction_count >= correction_words: break
    return {'completionWords': completion_count, 'completionQueries': query_count, 'prefixQueries': prefix_counts,
            'cleanCorrectionQueries': correction_count, 'deletionCorrectionQueries': correction_count,
            'sha256': digest_file(destination)}


def finalize_queries(output: Path, completion_words: int = 2500, correction_words: int = 1000):
    """Regenerate only evaluation queries after any protocol refinement."""
    manifest_path = output / 'manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    for split in ('validation', 'test'):
        manifest['queries'][split] = make_queries(output/(split+'.jsonl'), output/(split+'.tsv'),
                                                completion_words, correction_words, manifest['seed'])
    manifest['queryProtocol'] = 'v3: content-document groups; incomplete prefixes0/1/2/3; runtime French normalization; dot/newline context resets; one token NLL per prefix0'
    manifest_path.write_text(json.dumps(manifest,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
    return manifest['queries']


class WindowsJob:
    """Process-tree accounting and hard private-commit ceiling; no sampling guess."""
    def __init__(self, pid: int, limit: int):
        from ctypes import wintypes as w
        self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        class IO(ctypes.Structure):
            _fields_ = [(x, ctypes.c_ulonglong) for x in ('ReadOperationCount','WriteOperationCount','OtherOperationCount','ReadTransferCount','WriteTransferCount','OtherTransferCount')]
        class BASIC(ctypes.Structure):
            _fields_ = [('PerProcessUserTimeLimit',ctypes.c_longlong),('PerJobUserTimeLimit',ctypes.c_longlong),
                ('LimitFlags',w.DWORD),('MinimumWorkingSetSize',ctypes.c_size_t),('MaximumWorkingSetSize',ctypes.c_size_t),
                ('ActiveProcessLimit',w.DWORD),('Affinity',ctypes.c_size_t),('PriorityClass',w.DWORD),('SchedulingClass',w.DWORD)]
        class EXTENDED(ctypes.Structure):
            _fields_ = [('BasicLimitInformation',BASIC),('IoInfo',IO),('ProcessMemoryLimit',ctypes.c_size_t),
                ('JobMemoryLimit',ctypes.c_size_t),('PeakProcessMemoryUsed',ctypes.c_size_t),('PeakJobMemoryUsed',ctypes.c_size_t)]
        self.structure = EXTENDED
        self.kernel.CreateJobObjectW.restype = w.HANDLE
        self.kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p,w.LPCWSTR]
        self.kernel.OpenProcess.restype = w.HANDLE
        self.kernel.OpenProcess.argtypes = [w.DWORD,w.BOOL,w.DWORD]
        self.kernel.AssignProcessToJobObject.argtypes = [w.HANDLE,w.HANDLE]
        self.kernel.SetInformationJobObject.argtypes = [w.HANDLE,ctypes.c_int,ctypes.c_void_p,w.DWORD]
        self.kernel.QueryInformationJobObject.argtypes = [w.HANDLE,ctypes.c_int,ctypes.c_void_p,w.DWORD,ctypes.c_void_p]
        self.kernel.CloseHandle.argtypes = [w.HANDLE]
        self.handle = self.kernel.CreateJobObjectW(None,None)
        if not self.handle: raise ctypes.WinError(ctypes.get_last_error())
        settings = EXTENDED()
        settings.BasicLimitInformation.LimitFlags = 0x00002000 | 0x00000200 # KILL_ON_JOB_CLOSE, JOB_MEMORY
        settings.JobMemoryLimit = limit
        if not self.kernel.SetInformationJobObject(self.handle,9,ctypes.byref(settings),ctypes.sizeof(settings)):
            self.close(); raise ctypes.WinError(ctypes.get_last_error())
        process = self.kernel.OpenProcess(0x0100 | 0x0001,False,pid)
        try:
            if not process or not self.kernel.AssignProcessToJobObject(self.handle,process):
                self.close(); raise ctypes.WinError(ctypes.get_last_error())
        finally:
            if process: self.kernel.CloseHandle(process)
    def peak(self):
        value = self.structure()
        if not self.kernel.QueryInformationJobObject(self.handle,9,ctypes.byref(value),ctypes.sizeof(value),None):
            raise ctypes.WinError(ctypes.get_last_error())
        return int(value.PeakJobMemoryUsed)
    def close(self):
        if self.handle: self.kernel.CloseHandle(self.handle); self.handle = None


def run_process(command: list[str], output: Path, limit_bytes: int, timeout: float):
    output.mkdir(parents=True, exist_ok=True)
    started = time.perf_counter()
    job = None
    sampled_peak_rss = 0
    phases = []
    pending = b''
    position = 0
    def progress():
        nonlocal pending, position
        with (output/'stdout.log').open('rb') as log:
            log.seek(position);data=log.read();position=log.tell()
        pending += data
        while b'\n' in pending:
            line,pending=pending.split(b'\n',1)
            fields=line.rstrip(b'\r').decode('utf-8',errors='replace').split('\t')
            if len(fields)>=3 and fields[0]=='@progress':
                stage=fields[2]
                if not phases or phases[-1]['stage']!=stage:
                    phases.append({'stage':stage,'atSeconds':time.perf_counter()-started})
    with (output/'stdout.log').open('wb') as stdout, (output/'stderr.log').open('wb') as stderr:
        child = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                                 creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        try:
            if os.name == 'nt': job = WindowsJob(child.pid,limit_bytes)
            while child.poll() is None:
                progress()
                if time.perf_counter() - started > timeout:
                    child.kill(); child.wait(); raise TimeoutError('Benchmark wall-clock limit exceeded')
                if os.name != 'nt':
                    try:
                        status = Path(f'/proc/{child.pid}/status').read_text()
                        match = re.search(r'^VmHWM:\s+(\d+) kB',status,re.M)
                        if match: sampled_peak_rss = max(sampled_peak_rss,int(match[1])*1024)
                        if sampled_peak_rss > limit_bytes: child.kill(); child.wait(); raise MemoryError('Benchmark RSS ceiling exceeded')
                    except FileNotFoundError: pass
                time.sleep(.05)
            peak = job.peak() if job else sampled_peak_rss
            progress()
            elapsed=time.perf_counter()-started
            for index,phase in enumerate(phases):
                phase['secondsUntilNextStage']=(phases[index+1]['atSeconds'] if index+1<len(phases) else elapsed)-phase['atSeconds']
            result = {'command': command, 'executableSHA256': digest_file(Path(command[0])),
                'wallSeconds': elapsed, 'peakMemoryBytes': peak, 'phases':phases,
                'memoryMetric': 'Windows job peak private committed bytes (process tree)' if job else 'Linux sampled VmHWM RSS (native child)',
                'hardMemoryLimitBytes': limit_bytes, 'exitCode': child.returncode,
                'platform': platform.platform(), 'logicalCPUs': os.cpu_count()}
        finally:
            if child.poll() is None: child.kill(); child.wait()
            if job: job.close()
    (output/'run.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    if result['exitCode']: raise RuntimeError(f"Benchmark failed, see {output}: exit{result['exitCode']}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='action',required=True)
    prep = commands.add_parser('prepare')
    prep.add_argument('--corpus',type=Path,required=True);prep.add_argument('--output',type=Path,required=True)
    prep.add_argument('--seed',default='lvgl-fr-FR-2026-11-24-v1')
    prep.add_argument('--completion-words',type=int,default=2500);prep.add_argument('--correction-words',type=int,default=1000)
    prep.add_argument('--subset-mib',type=int,default=100)
    run = commands.add_parser('run')
    run.add_argument('--output',type=Path,required=True);run.add_argument('--memory-mib',type=int,default=2048)
    run.add_argument('--timeout',type=float,default=21600);run.add_argument('command',nargs=argparse.REMAINDER)
    finalize = commands.add_parser('queries')
    finalize.add_argument('--prepared',type=Path,required=True)
    finalize.add_argument('--completion-words',type=int,default=2500)
    finalize.add_argument('--correction-words',type=int,default=1000)
    args = parser.parse_args()
    if args.action == 'prepare':
        prepare(args.corpus,args.output,args.seed,args.completion_words,args.correction_words,args.subset_mib*1024*1024)
    elif args.action == 'queries':
        print(json.dumps(finalize_queries(args.prepared,args.completion_words,args.correction_words)))
    else:
        command = args.command[1:] if args.command and args.command[0] == '--' else args.command
        if not command: parser.error('Native benchmark command required after --')
        print(json.dumps(run_process(command,args.output,args.memory_mib*1024*1024,args.timeout)))


if __name__ == '__main__': main()
