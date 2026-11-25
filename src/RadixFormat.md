# NGTR v4 compressed radix model

The portable reader accepts fixed-record NGTR v2, compact byte-trie NGTR v3
with NGIX v1, and path-compressed NGTR v4 with NGIX v2. Existing models remain
readable. V4 is a byte-oriented radix/Patricia representation: a nonterminal,
unscored single-child chain becomes one label span. Word terminals, score and
backoff locations, and branch points remain explicit. Very long spans split at
256 bytes. Splits may occur inside a UTF-8 scalar; matching and autocorrect keep
Unicode scalar state across those splits.

All fixed-width integers are little endian and may be unaligned. ULEB encodings
must be minimal. Counts and subtree masses in v4 are unsigned 64-bit values;
legacy v2/v3 counts remain unsigned 32-bit values. Invalid ranges, integer
overflow, trailing bytes, child order, inconsistent masses or probability bounds
cause validation to fail before the borrowed model is used.

## Model bytes

The 16-byte header is `NGTR`, uint32 version `4`, uint32 physical-edge count,
and uint32 expanded logical byte-node count. Two physical roots come first:
combined n-grams at ID 0 and independent unigram spellings at ID 1.

Physical records follow in breadth-first edge order:

1. ULEB32 label length, zero for roots and 1–256 otherwise.
2. Exactly that many nonzero label bytes.
3. ULEB32 flags: `(child_count << 3) | terminal | probability | backoff`, where
   terminal is bit 0, probability bit 1, and backoff bit 2. A node has at most
   255 children, ordered by the first byte of each child's span.
4. ULEB64 exact positive terminal count when bit 0 is present.
5. Signed int16 log probability when bit 1 is present.
6. Signed int16 log backoff weight when bit 2 is present.

Children are consecutive physical records. Their first physical index is inferred
by summing preceding degrees from an initial next-child index of 2. No child
pointers or native object layouts are serialized.

The optional scored-model tag is probability bit 1 on the combined root with
log value zero. This represents a format tag, not a candidate probability.
The unigram root then carries the unknown-word bucket's probability. Neither
root has a terminal count or a backoff weight. All positive-count terminals in
a scored model carry a probability. A count-zero history may carry backoff
metadata without becoming a visible spelling candidate.

## Quantized scores

Finite probabilities and backoff weights use
`q = round(log2(value) * 256)`. The signed value `-32768` means exact zero;
presence is determined by the flags, so encoded zero means probability/weight
one. Probabilities greater than one and finite scores outside the signed range
are rejected. Backoff weights can exceed one after pruning and normalization.

The reader adds decoded log scores during backoff. A stored observed n-gram
probability already includes interpolation and is used directly; an absent
n-gram uses its history's backoff weight times the lower-order probability.
Missing histories fall back with weight one. Unknown spellings use the unknown
bucket, which is not exposed as a keyboard suggestion.

Rounding contributes at most half a score step per factor: approximately
0.136% relative error for one finite probability, or 0.407% for a probability
multiplied by two backoff weights. This describes encoding error; pruning and
training quality are evaluated separately.

## NGIX v2

The 16-byte header is `NGIX`, uint32 version `2`, uint32 NGTR byte length, and
uint32 physical-edge count. It is followed by one 12-byte checkpoint per group
of 32 physical edges. A checkpoint stores the NGTR record offset, inferred next
BFS child index, and offset into the following mass stream at that group's start.

Only physical nonleaf edges have mass-stream entries. Each entry contains its
ULEB64 exact subtree mass. Scored models append an int16 maximum log probability
among that edge and all its descendants. Root tag/unknown probabilities are
excluded from this bound. Leaves reuse their terminal count and optional stored
probability. The reader verifies every stored bound against its children before
using it for pruning. Subtree frequencies are not probability bounds.

This removes repeated mass entries and checkpoints for the many collapsed unary
nodes. The Python `layout/build_ngtr_index.py` and C++ encoder produce identical
indexes, including scored bounds and 64-bit frequencies. Fixed-record v2 models
already provide direct access and need no sidecar.

## Runtime access and prediction

The reader borrows immutable model/index bytes. They may reside in flash,
memory-mapped files, ordinary RAM or the existing shared ESP32 PSRAM cache. It
does not expand spans into heap nodes or allocate while predicting.

Hot traversals use a caller-owned `Model::Cursor` to reuse the current decoded
physical edge and nearby forward records. Interior bytes therefore share their
span metadata instead of repeatedly scanning a checkpoint. Cursors are local
to each query; the model retains no mutable lookup cache and remains shareable
between callers with independent scratch. Reset cursors after `Open`/`Close`.
This adds fixed temporary cursor state on the call stack, without changing the
serialized format or expanding the retained model.

Byte handles retain exact positions inside spans: roots are handles 0 and 1;
other handles are `2 + 256 * (physical_edge - 2) + span_position`. Handles are
opaque and sparse. `NodeCount()` reports expanded logical bytes, while
`PhysicalNodeCount()` reports serialized edges. A node's children are
`first_child + ordinal * child_stride`; callers must not iterate sparse handles
as dense ordinals. `FollowFrom` compares spans in bulk. Autocorrect traverses
their logical bytes and advances its edit-distance rows once per Unicode scalar,
including transpositions across span boundaries.

Unscored v4 models preserve the existing trigram/bigram/unigram priority and
frequency ranking. Scored models partition candidates into three disjoint
groups: observed trigrams, remaining observed bigrams, and remaining unigrams.
Each group has a constant backoff multiplier, allowing exact top-three merging
with validated maximum-probability pruning. A word observed in a higher-order
context is excluded from every lower group, even if it did not reach the current
top three. Equal scores use deterministic byte-lexical ordering.

Autocorrect retains the established frequency/edit-distance prior and applies
the trained contextual log-likelihood ratio, `log P(word|history) - log P(word)`.
Its correction confidence remains a heuristic confidence; it is not a calibrated
language-model probability. In particular, the aggregate unknown bucket is not
treated as the probability of retaining one exact unknown spelling.

Automatic correction also applies the same safety policy to v2/v3 and v4:
an exact retained spelling with a finite score is left to the user; an explicit
zero-probability context can still rule it out. A replacement must be one Unicode
scalar insertion, deletion or substitution, or one adjacent transposition. Its
heuristic score must exceed both keeping the original and the next candidate
by a factor of at least 1.10. The configured confidence threshold is applied
after these gates and is not changed automatically. Manual completion ranking
is unchanged. This favors keeping real words and ambiguous input; it cannot
guarantee that an out-of-vocabulary spelling is a typo, and deliberately declines
automatic replacements requiring several edits.

Public C suggestion masses remain uint32 for ABI compatibility. Exact 64-bit
counts are used through ranking and are saturated only at that facade. For
scored suggestions the reported mass is the conditional probability scaled to
the uint32 range; ordering uses log scores before that conversion.

## Offline encoder

`RadixModelBuilder.h` takes unique normalized UTF-8 n-gram keys, exact counts and
optional full conditional probabilities/backoff weights. It returns NGTR/NGIX
byte vectors and exact combined flash size. The builder is a desktop training
tool and is excluded from the on-device source closure. `ConvertToRadix` supports
lossless conversion of unscored Creator models with matching combined/unigram
roots. Independently authored roots that disagree are rejected by that helper;
the legacy reader still accepts them.
