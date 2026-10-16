#include "ai_provider_store.h"

#include "engine/core/data_path.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <string_view>
#include <utility>

namespace AiAssistant::ProviderStore
{
namespace
{
constexpr int kSchemaVersion = 1;

std::string Trim(std::string value)
{
    const auto not_space = [](unsigned char ch) { return std::isspace(ch) == 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

bool IsSlugChar(unsigned char ch)
{
    return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch >= 0x80; // keep CJK bytes
}

std::map<std::string, std::string> ToMap(const std::vector<std::pair<std::string, std::string>> &pairs)
{
    std::map<std::string, std::string> result;
    for (const auto &[key, value] : pairs)
        if (!value.empty())
            result[key] = value;
    return result;
}
} // namespace

std::filesystem::path StorePath()
{
    return metasequoia::data_directory() / "ai_providers.json";
}

std::optional<Entries> Load(const std::filesystem::path &explicit_path)
{
    const std::filesystem::path path = explicit_path.empty() ? StorePath() : explicit_path;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return std::nullopt;
    Entries entries;
    if (root.contains("providers") && root.at("providers").is_array())
    {
        for (const auto &item : root.at("providers"))
        {
            if (!item.is_object())
                continue;
            AiProvider entry;
            const auto read_string = [&item](const char *key, std::string &out) {
                if (item.contains(key) && item.at(key).is_string())
                    out = item.at(key).get<std::string>();
            };
            read_string("id", entry.id);
            read_string("name", entry.name);
            read_string("endpoint", entry.endpoint);
            read_string("token", entry.token);
            read_string("model", entry.model);
            if (!entry.id.empty())
                entries.push_back(std::move(entry));
        }
    }
    return entries;
}

bool Save(const Entries &entries, const std::filesystem::path &explicit_path)
{
    const std::filesystem::path path = explicit_path.empty() ? StorePath() : explicit_path;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    nlohmann::json root = {{"version", kSchemaVersion}, {"providers", nlohmann::json::array()}};
    for (const auto &entry : entries)
    {
        root.at("providers")
            .push_back({{"id", entry.id},
                        {"name", entry.name},
                        {"endpoint", entry.endpoint},
                        {"token", entry.token},
                        {"model", entry.model}});
    }
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
        if (!output)
            return false;
        const std::string text = root.dump();
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output)
        {
            output.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::error_code rename_ec;
    std::filesystem::rename(tmp, path, rename_ec);
    if (rename_ec)
    {
        // rename onto an existing file can fail on some filesystems; replace() handles it.
        std::error_code replace_ec;
        std::filesystem::remove(path, replace_ec);
        std::filesystem::rename(tmp, path, replace_ec);
        return !replace_ec;
    }
    return true;
}

std::string MakeProviderId(const std::string &name, const Entries &existing)
{
    std::string slug;
    for (unsigned char ch : Trim(name))
    {
        if (IsSlugChar(ch))
            slug.push_back(static_cast<char>(std::tolower(ch)));
        else if (!slug.empty() && slug.back() != '-')
            slug.push_back('-');
    }
    while (!slug.empty() && slug.back() == '-')
        slug.pop_back();
    if (slug.empty())
        slug = "provider";
    const auto taken = [&existing](const std::string &id) {
        return std::any_of(existing.begin(), existing.end(), [&id](const AiProvider &entry) { return entry.id == id; });
    };
    if (!taken(slug))
        return slug;
    for (int suffix = 2;; ++suffix)
    {
        const std::string candidate = slug + "-" + std::to_string(suffix);
        if (!taken(candidate))
            return candidate;
    }
}

Entries SeedFromLegacy(const std::vector<std::pair<std::string, std::string>> &default_endpoints,
                       const std::vector<std::pair<std::string, std::string>> &default_models,
                       const std::vector<std::pair<std::string, std::string>> &legacy_endpoints,
                       const std::vector<std::pair<std::string, std::string>> &legacy_models,
                       const std::vector<std::pair<std::string, std::string>> &legacy_tokens)
{
    const auto endpoints = ToMap(legacy_endpoints);
    const auto models = ToMap(legacy_models);
    const auto tokens = ToMap(legacy_tokens);
    Entries entries;
    for (const auto &[id, default_endpoint] : default_endpoints)
    {
        AiProvider entry;
        entry.id = id;
        // Display names stay ASCII here; the UI shows whatever the user renames them to.
        entry.name = id;
        entry.endpoint = default_endpoint;
        if (const auto found = endpoints.find(id); found != endpoints.end())
            entry.endpoint = found->second;
        if (const auto found = models.find(id); found != models.end())
            entry.model = found->second;
        if (const auto found = tokens.find(id); found != tokens.end())
            entry.token = found->second;
        entries.push_back(std::move(entry));
    }
    return entries;
}

void ResolveActive(AiProvider &active_out, std::string &endpoint, std::string &model, std::string &token,
                   const std::string &provider, const Entries &entries)
{
    active_out = AiProvider{};
    const auto found = std::find_if(entries.begin(), entries.end(),
                                    [&provider](const AiProvider &entry) { return entry.id == provider; });
    if (found == entries.end())
        return;
    active_out = *found;
    // Only registry-only providers resolve through the store. The five built-in slot ids keep
    // config.toml authoritative - the settings page still writes endpoint/model/token there, and
    // treating a possibly-stale store copy as the winner would pin old values over new ones.
    static const std::array<std::string_view, 5> kBuiltins{"deepseek", "openai", "siliconflow", "groq", "custom"};
    const bool builtin = std::find(kBuiltins.begin(), kBuiltins.end(), provider) != kBuiltins.end();
    if (builtin)
        return;
    if (!found->endpoint.empty())
        endpoint = found->endpoint;
    if (!found->model.empty())
        model = found->model;
    if (!found->token.empty())
        token = found->token;
}
} // namespace AiAssistant::ProviderStore