#pragma once

#include "config/ai_provider.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <utility>

namespace AiAssistant
{
// Persists the user's provider registry as ai_providers.json in the data directory - NOT in
// config.toml, whose template merge drops non-credential keys it does not know, which would
// delete user-added providers on every upgrade. HanaAgent keeps its provider plugins out of the
// app config for the same reason.
namespace ProviderStore
{
using Entries = std::vector<AiProvider>;

// Entries as stored. Nullopt when the file does not exist yet, so the caller can migrate the
// legacy config slots into a first generation of entries. A present-but-corrupt file also reads
// as nullopt: the next Save() rewrites it wholesale. An explicit path keeps tests off the real
// data directory.
std::optional<Entries> Load(const std::filesystem::path &path = {});

// Persists the whole list. Writing is atomic (tmp file + rename) so a crash never leaves a
// half-written store behind. Returns false when the data directory is unavailable.
bool Save(const Entries &entries, const std::filesystem::path &path = {});

std::filesystem::path StorePath();

// A stable, slug-shaped id from a display name ("MiniMax 中转" -> "minimax-"), unique among the
// existing entries. Ids stay compatible with the legacy ai_assistant.<field>_<id> slot keys.
std::string MakeProviderId(const std::string &name, const Entries &existing);

// Seeds the first generation of entries from the legacy per-slot config keys. Built-in slots keep
// their default endpoints; a slot whose legacy endpoint was overridden carries that value over,
// so nothing the user configured is lost by the migration.
Entries SeedFromLegacy(const std::vector<std::pair<std::string, std::string>> &default_endpoints,
                       const std::vector<std::pair<std::string, std::string>> &default_models,
                       const std::vector<std::pair<std::string, std::string>> &legacy_endpoints,
                       const std::vector<std::pair<std::string, std::string>> &legacy_models,
                       const std::vector<std::pair<std::string, std::string>> &legacy_tokens);

// Copies the active provider's endpoint/model/token over the legacy single fields so the request
// path keeps working unchanged. Unknown ids leave them as the config file had them.
void ResolveActive(AiProvider &active_out, std::string &endpoint, std::string &model, std::string &token,
                   const std::string &provider, const Entries &entries);
} // namespace ProviderStore
} // namespace AiAssistant