// The provider registry is the HanaAgent-style replacement for the five hard-coded provider
// slots: any number of OpenAI-compatible services, persisted in the data directory. These tests
// stay off the real data directory by passing explicit paths.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ai/ai_provider_store.h"
#include "tests/includes/test_framework.h"

#include <filesystem>
#include <fstream>

namespace
{
std::filesystem::path TempStorePath()
{
    const auto base = std::filesystem::temp_directory_path();
    static int counter = 0;
    return base / ("ai-provider-store-test-" + std::to_string(++counter) + ".json");
}
} // namespace

TEST_CASE(ai_provider_store_round_trips_entries)
{
    const auto path = TempStorePath();
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // Absent file reads as nullopt so the caller knows to seed from the legacy slots.
    const auto before = AiAssistant::ProviderStore::Load(path);
    REQUIRE(!before.has_value());

    AiAssistant::AiProvider minimax;
    minimax.id = "minimax";
    minimax.name = "MiniMax";
    minimax.endpoint = "https://api.minimax.cn/v1/chat/completions";
    minimax.token = "sk-test";
    minimax.model = "MiniMax-M3.1-Flash-Preview";
    AiAssistant::AiProvider deepseek;
    deepseek.id = "deepseek";
    deepseek.name = "DeepSeek";
    deepseek.endpoint = "https://api.deepseek.com/chat/completions";
    REQUIRE(AiAssistant::ProviderStore::Save({deepseek, minimax}, path));

    const auto loaded = AiAssistant::ProviderStore::Load(path);
    REQUIRE(loaded.has_value());
    REQUIRE_EQ(loaded->size(), static_cast<size_t>(2));
    REQUIRE_EQ((*loaded)[0].id, std::string("deepseek"));
    REQUIRE_EQ((*loaded)[1].token, std::string("sk-test"));
    REQUIRE_EQ((*loaded)[1].model, std::string("MiniMax-M3.1-Flash-Preview"));
    std::filesystem::remove(path, ec);
}

TEST_CASE(ai_provider_store_tolerates_a_corrupt_file)
{
    const auto path = TempStorePath();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    {
        std::ofstream output(path, std::ios::binary);
        output << "{ this is not json";
    }
    // Corrupt reads as absent: the next Save() rewrites the file wholesale.
    const auto loaded = AiAssistant::ProviderStore::Load(path);
    REQUIRE(!loaded.has_value());
    std::filesystem::remove(path, ec);
}

TEST_CASE(ai_provider_store_makes_unique_slug_ids)
{
    AiAssistant::ProviderStore::Entries existing;
    AiAssistant::AiProvider first;
    first.id = "minimax";
    existing.push_back(first);

    REQUIRE_EQ(AiAssistant::ProviderStore::MakeProviderId("MiniMax", existing), std::string("minimax-2"));
    // CJK 字节保留在 id 里（TOML 键合法），分隔符折叠成中划线。
    const auto slug = AiAssistant::ProviderStore::MakeProviderId("  OpenAI 中转 ", existing);
    REQUIRE(slug.rfind("openai", 0) == 0);
    REQUIRE_EQ(AiAssistant::ProviderStore::MakeProviderId("###", existing), std::string("provider"));
}

TEST_CASE(ai_provider_store_seeds_from_legacy_slots_without_losing_values)
{
    const std::vector<std::pair<std::string, std::string>> default_endpoints{
        {"deepseek", "https://api.deepseek.com/chat/completions"}, {"custom", ""}};
    const std::vector<std::pair<std::string, std::string>> default_models{{"deepseek", "deepseek-v4-flash"},
                                                                          {"custom", ""}};
    const std::vector<std::pair<std::string, std::string>> legacy_endpoints{
        {"deepseek", "https://api.minimax.cn/v1/chat/completions"}};
    const std::vector<std::pair<std::string, std::string>> legacy_models{{"deepseek", "MiniMax-M3.1-Flash-Preview"}};
    const std::vector<std::pair<std::string, std::string>> legacy_tokens{{"deepseek", "sk-kept"}};

    const auto seeded = AiAssistant::ProviderStore::SeedFromLegacy(default_endpoints, default_models, legacy_endpoints,
                                                                   legacy_models, legacy_tokens);
    REQUIRE_EQ(seeded.size(), static_cast<size_t>(2));
    // 用户改过的值随迁移保留——这正是用户把 MiniMax 填进 DeepSeek 那格的情形。
    REQUIRE_EQ(seeded[0].endpoint, std::string("https://api.minimax.cn/v1/chat/completions"));
    REQUIRE_EQ(seeded[0].model, std::string("MiniMax-M3.1-Flash-Preview"));
    REQUIRE_EQ(seeded[0].token, std::string("sk-kept"));
    // 没动过的槽位用默认值。
    REQUIRE(seeded[1].endpoint.empty());
}

TEST_CASE(ai_provider_store_resolves_the_active_entry)
{
    AiAssistant::ProviderStore::Entries entries;
    AiAssistant::AiProvider minimax;
    minimax.id = "minimax";
    minimax.endpoint = "https://api.minimax.cn/v1/chat/completions";
    minimax.model = "MiniMax-M3.1-Flash-Preview";
    minimax.token = "sk-active";
    entries.push_back(minimax);

    AiAssistant::AiProvider active;
    std::string endpoint = "https://api.deepseek.com/chat/completions";
    std::string model = "deepseek-v4-flash";
    std::string token;
    AiAssistant::ProviderStore::ResolveActive(active, endpoint, model, token, "minimax", entries);
    REQUIRE_EQ(endpoint, std::string("https://api.minimax.cn/v1/chat/completions"));
    REQUIRE_EQ(model, std::string("MiniMax-M3.1-Flash-Preview"));
    REQUIRE_EQ(token, std::string("sk-active"));
    REQUIRE_EQ(active.id, std::string("minimax"));

    // 未知 id：一切保持调用方的值（回退到 config.toml 的旧字段）。
    AiAssistant::ProviderStore::ResolveActive(active, endpoint, model, token, "ghost", entries);
    REQUIRE_EQ(endpoint, std::string("https://api.minimax.cn/v1/chat/completions"));
    REQUIRE(active.id.empty());

    // 内置槽位（deepseek/openai/siliconflow/groq/custom）的值以 config.toml 为准：
    // store 里的旧副本不得把新写入的值盖回去。
    entries.front().id = "deepseek";
    entries.front().endpoint = "https://stale.example.test/v1/chat/completions";
    std::string fresh_endpoint = "https://deepseek.example.test/chat/completions";
    AiAssistant::ProviderStore::ResolveActive(active, fresh_endpoint, model, token, "deepseek", entries);
    REQUIRE_EQ(fresh_endpoint, std::string("https://deepseek.example.test/chat/completions"));
    REQUIRE_EQ(active.id, std::string("deepseek"));
}