# Smoothed word prediction training

`TrainingModel.h` implements an offline, modified Kneser-Ney word predictor.
It consumes complete, globally merged unigram, bigram and trigram counts with
deterministic vocabulary IDs. ID zero is the unknown-word bucket. Training
operates on the host; the exported reader needs only the compact probabilities
and backoff weights.

The GUI defaults to compressed radix with Counts scoring. This module runs only
when Modified Kneser-Ney is selected explicitly (`--format radix --smoothing
modified-kn` on the native CLI). It adds training and metadata costs. French
validation has shown a next-word benefit at a larger profile alongside losses in
some small-profile completion/correction metrics. Choose using held-out results
for the intended language, byte budget and keyboard task; neither smoothing nor
compression alone establishes a quality improvement.

For trigram models, the highest-order count is the raw trigram count. The
lower-order bigram count is the number of distinct left words completing that
bigram, and the unigram count is its distinct left-context count from bigrams.
The per-order discounts are estimated from count-of-counts bins:

```
Y  = n1 / (n1 + 2*n2)
D1 = 1 - 2*Y*n2/n1
D2 = 2 - 3*Y*n3/n2
D3 = 3 - 4*Y*n4/n3
```

A missing/degenerate bin uses a 0.75 absolute-discount fallback for that class.
The fallback is reported in the returned discounts; it is not an estimate from
unavailable statistics. Unigrams interpolate with the uniform distribution over
the declared vocabulary, including the unknown bucket. Each higher-order
observed probability includes lower-order interpolation:

```
P(word | history) = max(count - discount, 0) / total(history)
                    + lambda(history) * P(word | shorter_history)
```

An unobserved entry uses only `lambda(history) * P(shorter_history)`. The
exported observed probability already includes interpolation, so readers must
not add its lower-order component twice.

The supplied corpus tokenizer and sentence-reset rules are preserved. Training
does not insert synthetic start/end-of-sentence markers, and is not equivalent
to training a complete sentence model with KenLM. Initial-context prediction,
next-word completion and out-of-vocabulary behavior should therefore be
reported separately from sentence-level perplexity.

## Memory and disk use

The trainer retains a 24-byte base record per vocabulary ID, then uses the
remaining configured RAM budget for fixed-size external-sort buffers. Its
temporary count and scored records are 24 and 48 bytes respectively. Eight-way
merge levels avoid retaining one path per corpus chunk. No complete bigram or
trigram hash table is created. Trigrams are sorted by their suffix for a merge
join with lower-order scores, then restored to context order; interpolation
does not perform a random disk lookup per trigram.

`Summary::peakWorkingBytes` tracks the reserved vocabulary/sort buffers plus an
I/O allowance. It is not the process's measured resident set. Benchmarks should
also record actual peak process memory, temporary bytes and cold/cached time.
The configured budget belongs to this training stage; callers must separately
account for retained vocabulary strings and any other live parent buffers.
The production generator reserves vocabulary/I/O workspace and divides the
global `--memory-mib` budget between stages and active workers. The trainer's
allowance is a share of that budget, not an extra per-worker allowance. This
does not impose an OS process-memory ceiling.

## Profile selection and pruning

`ScoredNGram::informationGain` is positive training log-likelihood gain over the
lower-order model, weighted by raw count. Dividing it by an estimated encoded
byte cost is a selection heuristic. It does not establish held-out
prediction improvement. Retain vocabulary and history dependencies with each
selected higher-order entry.

The production selector keeps a bounded candidate pool ranked by estimated
benefit per byte, includes required target/history vocabulary, and checks the
actual encoded NGTR+NGIX bytes against each radix budget. It is a heuristic,
not an exhaustive globally optimal subset search. One scored artifact can serve
several `--profile-sizes` budgets without repeating corpus counting or training.

After selecting a size profile, call `ReestimatePrunedBackoffs`. Known unigram
probabilities remain unchanged; omitted vocabulary mass is consolidated into
the unknown bucket. Each history is normalized against the *pruned* lower-order
distribution:

```
alpha(history) = (1 - sum(retained observed probabilities))
                 / (1 - sum(their lower-order probabilities))
```

Such weights may exceed one, which is why the radix format stores signed
quantized log scores. Complete lower-order support is normalized explicitly
instead of dividing by zero. History-only metadata is supported, and invalid
probabilities, duplicates and missing vocabulary/history dependencies are
rejected.

Evaluation must report vocabulary coverage/OOV rate along with completion
accuracy. Perplexity across different retained vocabularies can be misleading:
combining many missing words into one unknown bucket can lower its numerical
loss without improving prediction of those words.

The algorithms follow [the Kneser-Ney appendix in Speech and Language
Processing](https://web.stanford.edu/~jurafsky/slp3/C.pdf) and the streaming
estimation approach described by [Heafield et al., ACL
2013](https://aclanthology.org/P13-2121/).
