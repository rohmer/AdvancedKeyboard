#include "TrainingModel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string>
#include <utility>

namespace LanguageModels::Training {
namespace {
namespace fs = std::filesystem;
constexpr std::size_t ioReserve = 256u * 1024u;
constexpr std::size_t fanIn = 8;

struct Key {
    std::array<std::uint32_t, 3> words{};
    std::uint32_t order = 0;
};
bool Less(const Key& a, const Key& b) {
    return a.order != b.order ? a.order < b.order : a.words < b.words;
}
bool Equal(const Key& a, const Key& b) { return a.order == b.order && a.words == b.words; }
Key KeyOf(const NGram& value) { return {value.words, value.order}; }
struct CountRow { Key key; std::uint64_t count = 0; };
struct ScoreRow {
    Key key;
    std::uint64_t count = 0;
    double probability = 0, backoff = 1, gain = 0;
};
struct ContextRow {
    Key key;
    std::uint64_t total = 0;
    std::array<std::uint64_t, 3> types{};
};
struct Base { std::uint64_t count = 0; double probability = 0, backoff = 1; };
static_assert(sizeof(CountRow) == 24 && sizeof(ScoreRow) == 48, "Unexpected temporary-record layout");

std::uint64_t Add(std::uint64_t a, std::uint64_t b) {
    if (b > (std::numeric_limits<std::uint64_t>::max)() - a)
        throw std::overflow_error("Language model count overflow");
    return a + b;
}
struct Work {
    const Options& options;
    Summary summary;
    std::size_t baseBytes = 0, buffers = 0;
    void Check() const { if (options.check) options.check(); }
    void Peak() {
        summary.peakWorkingBytes = (std::max)(summary.peakWorkingBytes,
            std::uint64_t(baseBytes + buffers + ioReserve));
    }
    std::size_t Available() const {
        if (baseBytes + ioReserve >= options.memoryBytes)
            throw std::invalid_argument("Training RAM budget is too small for the vocabulary");
        return options.memoryBytes - baseBytes - ioReserve;
    }
};
class Scratch {
    fs::path parent_, directory_;
    std::uint64_t sequence_ = 0;
public:
    explicit Scratch(const Options& options) {
        static std::atomic<std::uint64_t> sequence{0};
        parent_ = options.scratchDirectory.empty() ? fs::temp_directory_path() / "LVGL_UI_Creator" / "LanguageModelWork" : options.scratchDirectory;
        fs::create_directories(parent_);
        parent_ = fs::weakly_canonical(fs::absolute(parent_));
        const auto name = ".mkn-work-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(sequence.fetch_add(1));
        directory_ = parent_ / name;
        if (directory_.parent_path() != parent_ || !fs::create_directory(directory_))
            throw std::runtime_error("Cannot create MKN training workspace");
    }
    ~Scratch() {
        // Only retire the exact directory created by this training invocation.
        // A replaced link/junction is preserved rather than followed.
        std::error_code error;
        const auto resolved = fs::weakly_canonical(directory_, error);
        if (!error && resolved == directory_ && resolved.parent_path() == parent_)
            fs::remove_all(directory_, error);
    }
    fs::path File() { return directory_ / (std::to_string(sequence_++) + ".run"); }
};
template<class Row> bool Read(std::istream& input, Row& row) {
    input.read(reinterpret_cast<char*>(&row), sizeof row);
    if (input.gcount() == 0 && input.eof() && !input.bad()) return false;
    if (!input || input.gcount() != sizeof row) throw std::runtime_error("Truncated MKN training run");
    return true;
}
template<class Row> void Write(std::ostream& output, const Row& row, Work& work) {
    output.write(reinterpret_cast<const char*>(&row), sizeof row);
    if (!output) throw std::runtime_error("Cannot write MKN training workspace");
    work.summary.temporaryBytesWritten = Add(work.summary.temporaryBytesWritten, sizeof row);
}
std::ifstream Input(const fs::path& path) {
    std::ifstream result(path, std::ios::binary);
    if (!result) throw std::runtime_error("Cannot open MKN training run: " + path.u8string());
    return result;
}
std::ofstream Output(const fs::path& path) {
    std::ofstream result(path, std::ios::binary | std::ios::trunc);
    if (!result) throw std::runtime_error("Cannot create MKN training run: " + path.u8string());
    return result;
}
void Close(std::ofstream& output) {
    output.flush();
    if (!output) throw std::runtime_error("Cannot flush MKN training workspace");
    output.close();
    if (!output) throw std::runtime_error("Cannot close MKN training workspace");
}

// A bounded external merge sort. Runs are merged in levels during ingestion,
// so the list of temporary paths grows logarithmically rather than per input
// chunk. Only aggregate-count streams merge equal keys.
template<class Row> class Sorter {
    Work& work_;
    Scratch& scratch_;
    std::size_t capacity_;
    bool aggregate_;
    std::vector<Row> rows_;
    std::vector<std::vector<fs::path>> levels_;
    fs::path Merge(const std::vector<fs::path>& files) {
        ++work_.summary.externalMergeGroups;
        struct Reader { std::ifstream input; Row row; explicit Reader(const fs::path& path) : input(Input(path)) {} };
        std::vector<std::unique_ptr<Reader>> readers;
        readers.reserve(files.size());
        for (const auto& file : files) readers.push_back(std::make_unique<Reader>(file));
        auto later = [&](std::size_t a, std::size_t b) { return Less(readers[b]->row.key, readers[a]->row.key); };
        std::priority_queue<std::size_t, std::vector<std::size_t>, decltype(later)> queue(later);
        for (std::size_t i = 0; i < readers.size(); ++i) if (Read(readers[i]->input, readers[i]->row)) queue.push(i);
        const auto path = scratch_.File();
        auto output = Output(path);
        Row pending{}; bool hasPending = false; std::uint64_t visited = 0;
        while (!queue.empty()) {
            if (!(visited++ & 4095)) work_.Check();
            const auto i = queue.top(); queue.pop();
            const auto& row = readers[i]->row;
            if (hasPending && aggregate_ && Equal(pending.key, row.key)) pending.count = Add(pending.count, row.count);
            else { if (hasPending) Write(output, pending, work_); pending = row; hasPending = true; }
            if (Read(readers[i]->input, readers[i]->row)) queue.push(i);
        }
        if (hasPending) Write(output, pending, work_);
        Close(output);
        readers.clear();
        for (const auto& file : files) fs::remove(file);
        return path;
    }
    void AddRun(fs::path file, std::size_t level) {
        if (levels_.size() <= level) levels_.resize(level + 1);
        levels_[level].push_back(std::move(file));
        if (levels_[level].size() == fanIn) {
            auto merged = Merge(levels_[level]); levels_[level].clear();
            AddRun(std::move(merged), level + 1);
        }
    }
    void Flush() {
        if (rows_.empty()) return;
        ++work_.summary.externalSortRuns;
        work_.Check();
        std::sort(rows_.begin(), rows_.end(), [](const Row& a, const Row& b) { return Less(a.key, b.key); });
        const auto path = scratch_.File(); auto output = Output(path);
        Row pending = rows_.front();
        for (std::size_t i = 1; i < rows_.size(); ++i) {
            if (!(i & 4095)) work_.Check();
            if (aggregate_ && Equal(pending.key, rows_[i].key)) pending.count = Add(pending.count, rows_[i].count);
            else { Write(output, pending, work_); pending = rows_[i]; }
        }
        Write(output, pending, work_); Close(output); rows_.clear();
        AddRun(path, 0);
    }
public:
    Sorter(Work& work, Scratch& scratch, bool aggregate) : work_(work), scratch_(scratch),
        capacity_(work.Available() / sizeof(Row)), aggregate_(aggregate) {
        if (capacity_ < 32) throw std::invalid_argument("Training RAM budget is too small for sorting");
        rows_.reserve(capacity_);
        work_.buffers += rows_.capacity() * sizeof(Row); work_.Peak();
    }
    ~Sorter() { work_.buffers -= rows_.capacity() * sizeof(Row); }
    void Push(Row row) {
        if (!(rows_.size() & 4095)) work_.Check();
        rows_.push_back(std::move(row)); if (rows_.size() == capacity_) Flush();
    }
    fs::path Finish() {
        Flush();
        work_.buffers -= rows_.capacity() * sizeof(Row);
        std::vector<Row>().swap(rows_);
        std::vector<fs::path> files;
        for (auto& level : levels_) for (auto& file : level) files.push_back(std::move(file));
        if (files.empty()) { const auto path = scratch_.File(); auto output = Output(path); Close(output); return path; }
        while (files.size() > 1) {
            std::vector<fs::path> next;
            for (std::size_t start = 0; start < files.size(); start += fanIn) {
                const auto end = (std::min)(files.size(), start + fanIn);
                if (end - start == 1) next.push_back(std::move(files[start]));
                else next.push_back(Merge({files.begin() + start, files.begin() + end}));
            }
            files = std::move(next);
        }
        return files.front();
    }
};

struct Histogram {
    std::array<std::uint64_t, 4> bins{};
    void Observe(std::uint64_t count) { if (count && count <= 4) ++bins[static_cast<std::size_t>(count - 1)]; }
    Discount Estimate() const {
        Discount result;
        if (!bins[0] || !bins[1]) return result;
        const double y = double(bins[0]) / (double(bins[0]) + 2.0 * double(bins[1]));
        auto valid = [](double value, double limit) { return std::isfinite(value) && value > 0 && value < limit ? value : .75; };
        result.one = valid(1 - 2 * y * double(bins[1]) / double(bins[0]), 1);
        if (bins[2]) result.two = valid(2 - 3 * y * double(bins[2]) / double(bins[1]), 2);
        if (bins[2] && bins[3]) result.threePlus = valid(3 - 4 * y * double(bins[3]) / double(bins[2]), 3);
        return result;
    }
};
Key History(const CountRow& row) {
    Key result = row.key;
    --result.order; result.words[row.key.order - 1] = 0;
    return result;
}
double Removed(const ContextRow& context, const Discount& discount) {
    return discount.one * double(context.types[0]) + discount.two * double(context.types[1]) +
        discount.threePlus * double(context.types[2]);
}
double Lambda(const ContextRow& context, const Discount& discount) {
    return context.total ? (std::min)(1.0, Removed(context, discount) / double(context.total)) : 1.0;
}
fs::path Contexts(const fs::path& source, Work& work, Scratch& scratch) {
    auto input = Input(source); const auto path = scratch.File(); auto output = Output(path);
    CountRow row; ContextRow pending; bool hasPending = false; std::uint64_t visited = 0;
    while (Read(input, row)) {
        if (!(visited++ & 4095)) work.Check();
        const auto key = History(row);
        if (hasPending && !Equal(key, pending.key)) { Write(output, pending, work); pending = {}; hasPending = false; }
        if (!hasPending) { pending = {}; pending.key = key; hasPending = true; }
        pending.total = Add(pending.total, row.count);
        ++pending.types[static_cast<std::size_t>((std::min)(std::uint64_t(3), row.count) - 1)];
    }
    if (hasPending) Write(output, pending, work);
    Close(output); return path;
}
double Gain(std::uint64_t count, double probability, double lower) {
    return count && probability > lower && lower > 0 ? double(count) * std::log(probability / lower) : 0;
}
ScoreRow Score(const Key& key, std::uint64_t count, double probability, double lower, double backoff = 1) {
    return {key, count, probability, backoff, Gain(count, probability, lower)};
}
ScoredNGram Public(const ScoreRow& row) {
    ScoredNGram result;
    result.words = row.key.words; result.order = static_cast<std::uint8_t>(row.key.order); result.count = row.count;
    result.probability = row.probability; result.backoff = row.backoff; result.informationGain = row.gain;
    return result;
}
void Validate(const NGram& value, std::uint32_t vocabularySize) {
    if (value.order < 1 || value.order > 3 || !value.count) throw std::invalid_argument("Invalid MKN n-gram count record");
    for (unsigned i = 0; i < 3; ++i)
        if ((i < value.order && value.words[i] >= vocabularySize) || (i >= value.order && value.words[i]))
            throw std::invalid_argument("MKN count record has an invalid word ID");
}
}

double Discount::operator()(std::uint64_t count) const noexcept {
    return count == 0 ? 0 : count == 1 ? one : count == 2 ? two : threePlus;
}

Summary TrainModifiedKneserNey(const CountScanner& scan, std::uint32_t vocabularySize,
                              const Options& options, const ScoreVisitor& output) {
    if (!scan || !output || !vocabularySize) throw std::invalid_argument("MKN training needs a vocabulary, scanner and output");
    if (options.memoryBytes < 1024u * 1024u) throw std::invalid_argument("MKN training requires at least 1 MiB of RAM budget");
    const auto denseBytes = std::uint64_t(vocabularySize) * sizeof(Base);
    if (denseBytes + ioReserve + 32 * sizeof(ScoreRow) > options.memoryBytes)
        throw std::invalid_argument("MKN training RAM budget is too small for the vocabulary");
    Work work{options}; work.summary.vocabularySize = vocabularySize; work.Check();
    Scratch scratch(options);
    std::array<fs::path, 3> raw;
    std::array<std::ofstream, 3> rawOutputs;
    for (unsigned i = 0; i < 3; ++i) { raw[i] = scratch.File(); rawOutputs[i] = Output(raw[i]); }
    Key previous{}; bool hasPrevious = false; std::uint64_t visited = 0;
    auto ingest = [&](const CountRow& row) {
        if (!(visited++ & 4095)) work.Check();
        if (hasPrevious && !Less(previous, row.key)) throw std::invalid_argument("MKN input must have unique sorted n-grams");
        previous = row.key; hasPrevious = true;
        Write(rawOutputs[row.key.order - 1], row, work);
        ++work.summary.records[row.key.order - 1];
    };
    if (options.sortedUniqueInput) {
        scan([&](const NGram& value) { Validate(value, vocabularySize); ingest({KeyOf(value), value.count}); });
    } else {
        fs::path sorted;
        {
            Sorter<CountRow> sorter(work, scratch, true);
            scan([&](const NGram& value) { Validate(value, vocabularySize); sorter.Push({KeyOf(value), value.count}); });
            sorted = sorter.Finish();
        }
        auto input = Input(sorted); CountRow row;
        while (Read(input, row)) ingest(row);
        input.close(); fs::remove(sorted);
    }
    for (auto& stream : rawOutputs) Close(stream);
    if (!work.summary.records[0]) throw std::invalid_argument("MKN training requires unigram counts");
    const bool hasBigrams = work.summary.records[1] != 0, hasTrigrams = work.summary.records[2] != 0;
    fs::path unigramCounts = raw[0], bigramCounts = raw[1];
    if (hasBigrams) {
        Sorter<CountRow> sorter(work, scratch, true); auto input = Input(raw[1]); CountRow row;
        while (Read(input, row)) sorter.Push({{{row.key.words[1], 0, 0}, 1}, 1});
        unigramCounts = sorter.Finish();
    }
    work.baseBytes = static_cast<std::size_t>(denseBytes); work.Peak();
    std::vector<Base> base(vocabularySize);
    Histogram unigramHistogram;
    std::uint64_t total = 0;
    {
        auto input = Input(unigramCounts); CountRow row;
        while (Read(input, row)) {
            work.Check(); base[row.key.words[0]].count = row.count; total = Add(total, row.count);
            unigramHistogram.Observe(row.count);
        }
    }
    work.summary.discounts[0] = unigramHistogram.Estimate();
    if (!total) throw std::invalid_argument("MKN training has no positive continuation counts");
    long double removed = 0;
    for (const auto& word : base) removed += work.summary.discounts[0](word.count);
    const double uniform = double(removed / static_cast<long double>(total)) / vocabularySize;
    for (auto& word : base)
        word.probability = (double(word.count) - work.summary.discounts[0](word.count)) / double(total) + uniform;
    work.summary.unknownProbability = base[0].probability;

    if (hasTrigrams) {
        Sorter<CountRow> sorter(work, scratch, true); auto input = Input(raw[2]); CountRow row;
        while (Read(input, row)) sorter.Push({{{row.key.words[1], row.key.words[2], 0}, 2}, 1});
        bigramCounts = sorter.Finish();
    }
    Histogram bigramHistogram, trigramHistogram;
    {
        auto input = Input(bigramCounts); CountRow row;
        while (Read(input, row)) bigramHistogram.Observe(row.count);
        auto trigrams = Input(raw[2]);
        while (Read(trigrams, row)) trigramHistogram.Observe(row.count);
    }
    work.summary.discounts[1] = bigramHistogram.Estimate();
    work.summary.discounts[2] = trigramHistogram.Estimate();
    const auto bigramContexts = Contexts(bigramCounts, work, scratch);
    {
        auto input = Input(bigramContexts); ContextRow context;
        while (Read(input, context)) base[context.key.words[0]].backoff = Lambda(context, work.summary.discounts[1]);
    }
    const auto trigramContexts = Contexts(raw[2], work, scratch);

    // Lower-order interpolation is sequential in context order. All raw
    // bigrams are emitted, including sentence-initial pairs with no distinct
    // left-context count, because they can carry a trigram history weight.
    const auto bigramScores = scratch.File();
    {
        auto rawInput = Input(raw[1]); auto adjusted = Input(bigramCounts); auto stats = Input(bigramContexts);
        auto out = Output(bigramScores);
        CountRow original, lower; ContextRow context;
        bool hasLower = Read(adjusted, lower), hasContext = Read(stats, context);
        while (Read(rawInput, original)) {
            work.Check();
            if (hasLower && Less(lower.key, original.key)) throw std::invalid_argument("MKN trigram suffix is missing from bigram counts");
            std::uint64_t count = 0;
            if (hasLower && Equal(lower.key, original.key)) { count = lower.count; hasLower = Read(adjusted, lower); }
            const auto history = History(original);
            while (hasContext && Less(context.key, history)) hasContext = Read(stats, context);
            const auto denominator = hasContext && Equal(context.key, history) ? context.total : 0;
            const auto& from = base[original.key.words[0]];
            const double lowerProbability = base[original.key.words[1]].probability;
            const double direct = denominator ? (double(count) - work.summary.discounts[1](count)) / double(denominator) : 0;
            Write(out, Score(original.key, original.count, direct + from.backoff * lowerProbability, lowerProbability), work);
        }
        if (hasLower) throw std::invalid_argument("MKN trigram suffix is missing from bigram counts");
        Close(out);
    }

    // Attach the normalized high-order term before sorting by suffix. That
    // makes interpolation a merge join instead of millions of random disk
    // lookups into a bigram table.
    fs::path suffixTrigrams;
    {
        Sorter<ScoreRow> sorter(work, scratch, false);
        auto input = Input(raw[2]); auto stats = Input(trigramContexts);
        CountRow row; ContextRow context; bool hasContext = Read(stats, context);
        while (Read(input, row)) {
            work.Check(); const auto history = History(row);
            while (hasContext && Less(context.key, history)) hasContext = Read(stats, context);
            if (!hasContext || !Equal(context.key, history) || !context.total)
                throw std::runtime_error("Missing MKN trigram context statistics");
            const Key suffix{{row.key.words[1], row.key.words[2], row.key.words[0]}, 3};
            sorter.Push({suffix, row.count,
                (double(row.count) - work.summary.discounts[2](row.count)) / double(context.total),
                Lambda(context, work.summary.discounts[2]), 0});
        }
        suffixTrigrams = sorter.Finish();
    }
    fs::path trigramScores;
    {
        Sorter<ScoreRow> sorter(work, scratch, false);
        auto input = Input(suffixTrigrams); auto lowerInput = Input(bigramScores);
        ScoreRow row, lower; bool hasLower = Read(lowerInput, lower);
        while (Read(input, row)) {
            work.Check(); const Key suffix{{row.key.words[0], row.key.words[1], 0}, 2};
            while (hasLower && Less(lower.key, suffix)) hasLower = Read(lowerInput, lower);
            if (!hasLower || !Equal(lower.key, suffix)) throw std::invalid_argument("MKN trigram suffix is missing from bigram counts");
            const double probability = row.probability + row.backoff * lower.probability;
            const Key original{{row.key.words[2], row.key.words[0], row.key.words[1]}, 3};
            sorter.Push(Score(original, row.count, probability, lower.probability));
        }
        trigramScores = sorter.Finish();
    }

    // Emit all vocabulary probabilities, then bigrams/trigrams. Raw counts are
    // retained for export/provenance; adjusted continuation counts are internal.
    {
        auto input = Input(raw[0]); CountRow row; bool has = Read(input, row);
        for (std::uint32_t id = 0; id < vocabularySize; ++id) {
            if (!(id & 4095)) work.Check();
            std::uint64_t count = 0;
            if (has && row.key.words[0] == id) { count = row.count; has = Read(input, row); }
            output(Public(Score({{id, 0, 0}, 1}, count, base[id].probability, 1.0 / vocabularySize, base[id].backoff)));
        }
    }
    {
        auto input = Input(bigramScores); auto stats = Input(trigramContexts);
        ScoreRow row; ContextRow context; bool hasContext = Read(stats, context);
        while (Read(input, row)) {
            work.Check();
            if (hasContext && Less(context.key, row.key)) throw std::invalid_argument("MKN trigram history is missing from bigram counts");
            if (hasContext && Equal(context.key, row.key)) { row.backoff = Lambda(context, work.summary.discounts[2]); hasContext = Read(stats, context); }
            output(Public(row));
        }
        if (hasContext) throw std::invalid_argument("MKN trigram history is missing from bigram counts");
    }
    {
        auto input = Input(trigramScores); ScoreRow row;
        while (Read(input, row)) { work.Check(); output(Public(row)); }
    }
    work.Check(); return work.summary;
}

const ScoredNGram* Model::Find(unsigned order, std::array<std::uint32_t, 3> words) const {
    const Key key{words, order};
    const auto found = std::lower_bound(records_->begin(), records_->end(), key,
        [](const ScoredNGram& value, const Key& query) { return Less(KeyOf(value), query); });
    return found != records_->end() && Equal(KeyOf(*found), key) ? &*found : nullptr;
}
Model::Model(const std::vector<ScoredNGram>& records, std::uint32_t unknownId) : records_(&records), unknown_(unknownId) {
    if (records.empty() || !std::is_sorted(records.begin(), records.end(),
        [](const ScoredNGram& a, const ScoredNGram& b) { return Less(KeyOf(a), KeyOf(b)); }) || !Find(1, {unknown_, 0, 0}))
        throw std::invalid_argument("A scored model needs sorted records and an unknown-word unigram");
}
double Model::Probability(std::uint32_t target, std::uint32_t previous, std::uint32_t older, unsigned historyLength) const {
    if (!Find(1, {target, 0, 0})) target = unknown_;
    const auto* unigram = Find(1, {target, 0, 0});
    double result = unigram->probability;
    if (historyLength > 0) {
        if (const auto* observed = Find(2, {previous, target, 0})) result = observed->probability;
        else if (const auto* history = Find(1, {previous, 0, 0})) result *= history->backoff;
    }
    if (historyLength > 1) {
        if (const auto* observed = Find(3, {older, previous, target})) result = observed->probability;
        else if (const auto* history = Find(2, {older, previous, 0})) result *= history->backoff;
    }
    return result;
}

double ReestimatePrunedBackoffs(std::vector<ScoredNGram>& retained, std::uint32_t unknownId) {
    auto sort = [&] { std::sort(retained.begin(), retained.end(), [](const ScoredNGram& a, const ScoredNGram& b) { return Less(KeyOf(a), KeyOf(b)); }); };
    sort();
    long double known = 0; bool hasUnknown = false;
    for (std::size_t i = 0; i < retained.size(); ++i) {
        auto& row = retained[i];
        if (row.order < 1 || row.order > 3 || !std::isfinite(row.probability) || row.probability < 0 || row.probability > 1 ||
            (row.count && row.probability == 0) || !std::isfinite(row.backoff) || row.backoff < 0 ||
            (i && Equal(KeyOf(retained[i - 1]), KeyOf(row))))
            throw std::invalid_argument("Invalid or duplicate retained language-model record");
        for (unsigned word = row.order; word < 3; ++word)
            if (row.words[word]) throw std::invalid_argument("Invalid trailing word ID in retained language model");
        if (row.order == 1) {
            row.backoff = 1;
            if (row.words[0] == unknownId) hasUnknown = true;
            else known += row.probability;
        } else row.backoff = 1;
    }
    if (known > 1 + 1e-10L) throw std::invalid_argument("Retained unigram probability exceeds one");
    const double unknown = double((std::max)(0.0L, 1.0L - known));
    if (!hasUnknown) { ScoredNGram row; row.order = 1; row.words[0] = unknownId; row.probability = unknown; retained.push_back(row); sort(); }
    auto find = [&](unsigned order, std::array<std::uint32_t, 3> words) -> ScoredNGram* {
        const Key key{words, order};
        auto it = std::lower_bound(retained.begin(), retained.end(), key,
            [](const ScoredNGram& value, const Key& query) { return Less(KeyOf(value), query); });
        return it != retained.end() && Equal(KeyOf(*it), key) ? &*it : nullptr;
    };
    find(1, {unknownId, 0, 0})->probability = unknown;
    for (const auto& row : retained) if (row.order > 1) {
        for (unsigned i = 0; i < row.order; ++i)
            if (!find(1, {row.words[i], 0, 0})) throw std::invalid_argument("Pruned n-gram has no retained vocabulary word");
        if (row.order == 3 && !find(2, {row.words[0], row.words[1], 0}))
            throw std::invalid_argument("Pruned trigram has no retained history metadata");
    }
    Model model(retained, unknownId);
    for (unsigned order = 2; order <= 3; ++order) {
        auto begin = std::lower_bound(retained.begin(), retained.end(), Key{{}, order},
            [](const ScoredNGram& value, const Key& query) { return Less(KeyOf(value), query); });
        while (begin != retained.end() && begin->order == order) {
            auto end = begin + 1;
            while (end != retained.end() && end->order == order && end->words[0] == begin->words[0] &&
                   (order == 2 || end->words[1] == begin->words[1])) ++end;
            long double observed = 0, lower = 0;
            for (auto row = begin; row != end; ++row) if (row->probability > 0) {
                observed += row->probability;
                lower += order == 2 ? model.Probability(row->words[1], 0, 0, 0) :
                    model.Probability(row->words[2], row->words[1], 0, 1);
            }
            if (observed > 1 + 1e-10L || lower > 1 + 1e-10L)
                throw std::invalid_argument("Retained conditional probabilities exceed one");
            double alpha = 1;
            if (1 - lower <= 1e-12L) {
                // Complete lower-order support leaves no unseen destination.
                // Normalize observed values rather than dividing by zero.
                if (observed <= 0) throw std::invalid_argument("Conditional model has no probability mass");
                for (auto row = begin; row != end; ++row) row->probability /= double(observed);
                alpha = 0;
            } else alpha = double((std::max)(0.0L, 1 - observed) / (1 - lower));
            auto* history = order == 2 ? find(1, {begin->words[0], 0, 0}) : find(2, {begin->words[0], begin->words[1], 0});
            history->backoff = alpha;
            // A context-only row starts with zero probability. Materialize the
            // unchanged lower-order value so it can also serve as a history.
            for (auto row = begin; row != end; ++row) if (row->probability == 0 && row->count == 0) {
                const double probability = order == 2 ? model.Probability(row->words[1], 0, 0, 0) :
                    model.Probability(row->words[2], row->words[1], 0, 1);
                row->probability = alpha * probability;
            }
            begin = end;
        }
    }
    return unknown;
}
}
