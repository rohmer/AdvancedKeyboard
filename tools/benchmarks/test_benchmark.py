import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('language_benchmark',Path(__file__).with_name('benchmark.py'))
bench = importlib.util.module_from_spec(spec);spec.loader.exec_module(bench)
sys.path.insert(0,str(Path(__file__).parent))
import compare as comparison
import prepare_runtime_confirmation as confirmation

def crawl(path,records):
    with path.open('wb') as output:
        for record in [{'Version':2,'Start':'https://example.test'},*records]:
            raw = json.dumps(record).encode('utf-8')
            output.write(struct.pack('<i',len(raw)));output.write(raw)

class BenchmarkTests(unittest.TestCase):
    def test_confirmation_excludes_all_exposed_documents_and_preserves_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);prepared=root/'prepared';prepared.mkdir()
            (prepared/'manifest.json').write_text('{}')
            # Even a document header with no emitted query is considered exposed.
            (prepared/'test.tsv').write_text('#document\texposed\n#document\tempty-trailer\n')
            (prepared/'validation.tsv').write_text('#document\tvalidation\n')
            source=prepared/'test.jsonl'
            source.write_text(''.join(json.dumps({'document':document,'text':text})+'\n' for document,text in (
                ('exposed','Une ancienne phrase existe.'),('empty-trailer','Encore une ancienne phrase.'),
                ('fresh','Cette nouvelle phrase française contient plusieurs mots.'))),encoding='utf-8')
            before={p.name:bench.digest_file(p) for p in prepared.iterdir()}
            result=confirmation.prepare(prepared,root/'confirmation',completion_words=5,correction_words=2)
            self.assertEqual(result['oldExposedDocumentCount'],2)
            self.assertEqual(result['counts']['excludedExposedSentences'],2)
            self.assertEqual(confirmation.document_ids(root/'confirmation/confirmation.tsv'),{'fresh'})
            self.assertEqual(before,{p.name:bench.digest_file(p) for p in prepared.iterdir()})
            with self.assertRaises(ValueError):confirmation.prepare(prepared,prepared/'forbidden')
            with self.assertRaises(ValueError):confirmation.prepare(prepared,root/'confirmation')

    def test_content_grouping_does_not_depend_on_url(self):
        self.assertEqual(bench.split_for_document('identical body','seed'),bench.split_for_document('identical body','seed'))
        self.assertEqual(bench.normalized_sentence('  L’ÉCOLE  e\u0301TÉ. '),"l'école été.")
        self.assertEqual(bench.tokens("L’école d’été, 123 — était belle."),["l'école","d'été","123","était","belle"])
        self.assertEqual(bench.contextual_tokens('M. Dupont dort.'),[('m','',''),('dupont','',''),('dort','dupont','')])

    def test_preparation_purges_exact_heldout_duplicates_and_preserves_input(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary);corpus=directory/'corpus';corpus.mkdir()
            seed='test-seed';heldout=None;training=None
            for index in range(1000):
                sentence=f'La maison numéro {index} est assez grande.'
                split=bench.split_for_document(bench.normalized_sentence(sentence),seed)
                if split=='test' and heldout is None: heldout=sentence
                if split=='train' and training is None: training=sentence
                if heldout and training: break
            checkpoint=corpus/'sample.txt.crawl'
            crawl(checkpoint,[{'Url':'https://example.test/test','Page':{'Sentences':[heldout]}},
                {'Url':'https://example.test/mirror','Page':{'Sentences':[heldout]}},
                {'Url':'https://example.test/train','Page':{'Sentences':[training]}}])
            original=corpus/'wiki.txt';original.write_text(heldout+'\n'+training+'\n'+training.upper()+'\n',encoding='utf-8')
            before={p.name:bench.digest_file(p) for p in corpus.iterdir()}
            with contextlib.redirect_stdout(io.StringIO()):
                report=bench.prepare(corpus,directory/'prepared',seed,4,2)
            self.assertEqual(before,{p.name:bench.digest_file(p) for p in corpus.iterdir()})
            text=''.join(p.read_text(encoding='utf-8') for p in (directory/'prepared/train').glob('*.txt'))
            self.assertNotIn(heldout,text);self.assertEqual(text,training+'\n')
            self.assertEqual(report['counts']['duplicate_documents'],1)
            self.assertEqual(report['counts']['purged_heldout_overlaps'],1)
            self.assertEqual(report['counts']['duplicate_training_sentences'],1)
            self.assertGreater(report['queries']['test']['completionQueries'],0)
            self.assertIn('article IDs',report['wikipediaSplitLimitation'])
            with self.assertRaises(ValueError): bench.prepare(corpus,directory/'prepared',seed,4,2)

    def test_corrupt_crawl_does_not_mutate_or_truncate_input(self):
        with tempfile.TemporaryDirectory() as temporary:
            path=Path(temporary)/'broken.crawl';path.write_bytes(struct.pack('<i',100)+b'{}')
            before=path.read_bytes()
            with self.assertRaises(ValueError): list(bench.crawl_records(path))
            self.assertEqual(path.read_bytes(),before)

    def test_memory_limited_native_runner_records_exit_and_peak(self):
        with tempfile.TemporaryDirectory() as temporary:
            result=bench.run_process([sys.executable,'-c',
                                     "import time; print('@progress\\t0\\tCounting',flush=True); data=bytearray(4000000); time.sleep(.3); print('@progress\\t100\\tComplete',flush=True)"],
                                     Path(temporary),128*1024*1024,10)
            self.assertEqual(result['exitCode'],0)
            self.assertGreater(result['peakMemoryBytes'],4_000_000)
            self.assertEqual(result['hardMemoryLimitBytes'],128*1024*1024)
            self.assertEqual([phase['stage'] for phase in result['phases']],['Counting','Complete'])

    def test_queries_never_include_the_entire_expected_word(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory=Path(temporary);source=directory/'heldout.jsonl';target=directory/'queries.tsv'
            source.write_text(json.dumps({'document':'document-1','text':'À Paris il fait beau.'})+'\n',encoding='utf-8')
            result=bench.make_queries(source,target,5,0,'seed')
            rows=[line.split('\t') for line in target.read_text(encoding='utf-8').splitlines() if not line.startswith('#')]
            self.assertEqual(len(rows),result['completionQueries'])
            for kind,expected,previous,older,prefix,typed in rows:
                self.assertLess(len(prefix),len(expected));self.assertEqual(len(prefix),int(kind[1]))
            self.assertIn('#document\tdocument-1',target.read_text(encoding='utf-8'))

    def test_document_clustered_comparison_is_paired_and_verifies_hashes(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory=Path(temporary)
            paths=[]
            for name,hit in (('baseline',0),('candidate',1)):
                outcome=directory/(name+'-outcomes.tsv')
                text='row\tkind\tdocument\tknown\ttop1\ttop3\treciprocal\tcorrected\tchanged\tnll\n'
                text+=''.join(f'{index}\tp0\tdoc-{index}\t1\t{hit}\t{hit}\t{hit}\t0\t0\t-1\n' for index in range(12))
                outcome.write_text(text,encoding='utf-8')
                metadata=directory/(name+'.json')
                metadata.write_text(json.dumps({'outcomesSHA256':bench.digest_file(outcome),'queriesSHA256':'same',
                                                 'model_bytes':100,'index_bytes':20}),encoding='utf-8')
                paths.append(metadata)
            result=comparison.compare(*paths,repeats=100)
            self.assertEqual(result['metrics']['completionTop3']['delta'],1)
            self.assertEqual(result['metrics']['completionTop3']['delta95CI'],[1,1])
            (directory/'candidate-outcomes.tsv').write_text('tampered',encoding='utf-8')
            with self.assertRaises(ValueError):comparison.compare(*paths,repeats=100)

if __name__=='__main__': unittest.main()
