#include "../../core/data_path.h"
#include "../../japanese/japanese_sentence_decoder.h"
#include "../../providers/japanese_candidate_provider.h"
#include "../../schemes/japanese_romaji_scheme.h"
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace
{
void require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}

void make_model(const std::filesystem::path &path)
{
    struct Lemma
    {
        std::string reading;
        std::string surface;
        std::uint32_t cost;
    };
    std::vector<Lemma> lemmas = {{"か", "蚊", 100},          {"かわ", "川", 7000},           {"かわいい", "可愛い", 5},
                                 {"しんよう", "信用", 4000}, {"しんようそ", "新要素", 1},    {"は", "は", 100},
                                 {"はし", "橋", 4500},       {"はし", "箸", 5000},           {"はしる", "走る", 1},
                                 {"わたし", "私", 2000},     {"わたしはしる", "私は走る", 1}};
    std::stable_sort(lemmas.begin(), lemmas.end(),
                     [](const Lemma &a, const Lemma &b) { return a.reading < b.reading; });
    std::string strings;
    for (const auto &lemma : lemmas)
        strings += lemma.reading + lemma.surface;

    // A tiny MSJPDT1 model with one connection class. Cheap longer words deliberately
    // outrank exact readings by lexical cost, so this checks conversion versus prediction.
    std::ofstream output(path, std::ios::binary);
    const auto integer = [&](std::uint64_t value, int width) {
        for (int i = 0; i < width; ++i)
            output.put(static_cast<char>((value >> (i * 8)) & 255));
    };
    const std::uint64_t connection_offset = 56 + 20 * lemmas.size();
    output.write("MSJPDT1", 8);
    integer(1, 4);
    integer(lemmas.size(), 4);
    integer(1, 4);
    integer(0, 4);
    integer(56, 8);
    integer(connection_offset, 8);
    integer(connection_offset + 2, 8);
    integer(strings.size(), 8);
    std::uint64_t offset = 0;
    for (const auto &lemma : lemmas)
    {
        integer(offset, 4);
        integer(lemma.reading.size(), 2);
        integer(offset + lemma.reading.size(), 4);
        integer(lemma.surface.size(), 2);
        integer(0, 2);
        integer(0, 2);
        integer(lemma.cost, 4);
        offset += lemma.reading.size() + lemma.surface.size();
    }
    integer(0, 2);
    output << strings;
    require(output.good(), "Could not write Japanese model fixture.");
}

std::vector<WordItem> query(JapaneseCandidateProvider &provider, const std::string &romaji)
{
    JapaneseRomajiScheme scheme;
    scheme.set_raw_input(romaji, romaji);
    return provider.query(scheme.build_request());
}

size_t position(const std::vector<WordItem> &candidates, const std::string &word)
{
    const auto found =
        std::find_if(candidates.begin(), candidates.end(), [&](const WordItem &item) { return item.word == word; });
    require(found != candidates.end(), "Expected Japanese candidate is missing.");
    return static_cast<size_t>(found - candidates.begin());
}

void run_test(const std::filesystem::path &root)
{
    std::filesystem::create_directories(root);
    const auto model = root / "dict_japanese.dat";
    const auto database = root / "msime.db";
    make_model(model);
    require(japanese::JapaneseSentenceDecoder(metasequoia::path_to_utf8(model)).ready(),
            "Japanese model fixture did not load.");
    sqlite3 *db = nullptr;
    require(sqlite3_open(metasequoia::path_to_utf8(database).c_str(), &db) == SQLITE_OK,
            "Could not open Japanese dictionary fixture.");
    const int status =
        sqlite3_exec(db,
                     "CREATE TABLE japanese_lexicon(code TEXT,value TEXT,weight INTEGER,PRIMARY KEY(code,value));"
                     "INSERT INTO japanese_lexicon VALUES('nihongo','日本語',100);",
                     nullptr, nullptr, nullptr);
    sqlite3_close(db);
    require(status == SQLITE_OK, "Could not populate Japanese dictionary fixture.");

    JapaneseCandidateProvider provider(metasequoia::path_to_utf8(database), metasequoia::path_to_utf8(model));
    const auto hashi = query(provider, "hashi");
    require(position(hashi, "橋") < position(hashi, "走る"), "Prediction displaced exact reading 橋.");
    require(position(hashi, "箸") < position(hashi, "走る"), "Prediction displaced exact reading 箸.");
    const auto shinyou = query(provider, "shin'you");
    require(position(shinyou, "信用") < position(shinyou, "新要素"), "Prediction displaced exact reading 信用.");
    const auto sentence = query(provider, "watashiha");
    require(position(sentence, "私は") < position(sentence, "私は走る"),
            "Prediction displaced a complete reading spanning multiple lemmas.");
    // Lemmas covering only a prefix of the reading (川, 蚊) are not conversions of
    // what was typed, so they must not push the whole-word prediction down.
    const auto kawai = query(provider, "kawai");
    require(position(kawai, "可愛い") < position(kawai, "川"), "Prefix fragment 川 outranked a prediction.");
    require(position(kawai, "可愛い") < position(kawai, "蚊"), "Prefix fragment 蚊 outranked a prediction.");
    require(hashi[position(hashi, "走る")].weight <= hashi[position(hashi, "箸")].weight,
            "A prediction outweighs the exact conversion ranked above it.");

    const auto pending = query(provider, "kaw");
    require(!pending.empty() && pending.front().word == "可愛い",
            "An unfinished consonant lost its phrase predictions.");
    const auto kana = query(provider, "ka");
    require(kana.size() >= 2 && kana[0].word == "か" && kana[1].word == "カ",
            "Single-kana candidates lost their priority.");
    const auto hyphen = query(provider, "-");
    require(hyphen.size() == 2 && hyphen[0].word == "ー" && hyphen[1].word == "-",
            "Long-vowel key changed its candidates.");
    require(std::count_if(hashi.begin(), hashi.end(), [](const WordItem &item) { return item.word == "橋"; }) == 1,
            "An exact reading was duplicated by prefix predictions.");

    JapaneseCandidateProvider without_model(metasequoia::path_to_utf8(database),
                                            metasequoia::path_to_utf8(root / "missing.dat"));
    const auto dictionary_only = query(without_model, "nihongo");
    require(position(dictionary_only, "日本語") == 0,
            "Dictionary-only conversion stopped working without the optional model.");

    // 方案默认是 Quanpin：漏标的日文候选会被调频当拼音词写进拼音日志（#869）。
    const auto japanese_scheme = [](const std::vector<WordItem> &items) {
        return std::all_of(items.begin(), items.end(),
                           [](const WordItem &item) { return item.scheme == SchemeType::JapaneseRomaji; });
    };
    require(japanese_scheme(hashi) && japanese_scheme(kana) && japanese_scheme(hyphen) &&
                japanese_scheme(dictionary_only),
            "A Japanese candidate was left tagged with the default pinyin scheme.");
    const auto found = without_model.find_candidate(SchemeType::JapaneseRomaji, "nihongo", "日本語");
    require(found.has_value() && found->scheme == SchemeType::JapaneseRomaji,
            "find_candidate returned a Japanese candidate without its scheme.");
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("msime-japanese-candidates-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    int result = 0;
    try
    {
        run_test(root);
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        result = 1;
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return result;
}
