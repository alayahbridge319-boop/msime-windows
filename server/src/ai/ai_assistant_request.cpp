#include "ai_assistant_request.h"

#include "voice-input/voice_providers.h"

namespace AiAssistant::detail
{
nlohmann::json BuildRequestBody(const Request &request)
{
    const auto &config = request.config;
    // Which vendor answers is a property of the endpoint, not of the label in the settings UI: a
    // slot can hold any OpenAI-compatible endpoint, and DeepSeek's thinking.type makes MiniMax
    // reject the request outright instead of silently returning no suggestion.
    const auto vendor = VoiceInput::VendorForRequest(config.provider, config.endpoint);
    const bool thinking = vendor == VoiceInput::ChatVendor::DeepSeek &&
                          VoiceInput::IsValidThinkingLevel(config.thinking) && config.thinking != "disabled";
    nlohmann::json input = {{"segmented_pinyin", request.pinyin_segments},
                            {"context", request.context},
                            {"candidate_limit", config.candidate_limit}};
    nlohmann::json body = {
        {"model", config.model},
        {"stream", false},
        {"temperature", 0.2},
        // Reasoning tokens may count against max_tokens; 512 would cut the JSON answer off.
        {"max_tokens", thinking ? 8192 : 512},
        {"response_format", {{"type", "json_object"}}},
        {"messages",
         {{{"role", "system"}, {"content", config.prompt}}, {{"role", "user"}, {"content", input.dump()}}}}};
    if (thinking)
    {
        body["thinking"] = {{"type", "enabled"}};
        body["reasoning_effort"] = config.thinking;
    }
    else if (vendor == VoiceInput::ChatVendor::DeepSeek)
    {
        // DeepSeek thinking can add hundreds of reasoning tokens and noticeably delay
        // an IME suggestion. Every other service gets a plain OpenAI-compatible request.
        body["thinking"] = {{"type", "disabled"}};
    }
    return body;
}

long RequestTimeoutMs(const AiAssistantConfig &config)
{
    return VoiceInput::UsesDeepSeekThinking(config.provider, config.endpoint, config.thinking) ? 30000L : 8000L;
}
} // namespace AiAssistant::detail
