#include "ai/ai_assistant_request.h"
#include "tests/includes/test_framework.h"

namespace
{
// 厂商字段现在按接口地址判断，所以构造请求时必须显式给地址；默认给一个 DeepSeek 地址，
// 让"标签=deepseek"的历史用例保持原意。
AiAssistant::Request MakeRequest(const std::string &provider, const std::string &thinking,
                                 const std::string &endpoint = "https://api.deepseek.com/chat/completions")
{
    AiAssistant::Request request;
    request.pinyin_segments = {"shang", "hai"};
    request.context = "我准备去";
    request.config.provider = provider;
    request.config.model = "example-model";
    request.config.endpoint = endpoint;
    request.config.prompt = "Return the best candidate.";
    request.config.thinking = thinking;
    return request;
}
} // namespace

TEST_CASE(ai_request_disables_deepseek_thinking_by_default)
{
    const auto body = AiAssistant::detail::BuildRequestBody(MakeRequest("deepseek", "disabled"));
    REQUIRE_EQ(body["thinking"]["type"].get<std::string>(), "disabled");
    REQUIRE(!body.contains("reasoning_effort"));
    REQUIRE_EQ(body["max_tokens"].get<int>(), 512);
    REQUIRE_EQ(AiAssistant::detail::RequestTimeoutMs(MakeRequest("deepseek", "disabled").config), 8000L);
}

TEST_CASE(ai_request_sends_deepseek_thinking_level)
{
    for (const char *level : {"low", "high", "max"})
    {
        const auto request = MakeRequest("deepseek", level);
        const auto body = AiAssistant::detail::BuildRequestBody(request);
        REQUIRE_EQ(body["thinking"]["type"].get<std::string>(), "enabled");
        REQUIRE_EQ(body["reasoning_effort"].get<std::string>(), std::string(level));
        REQUIRE(body["max_tokens"].get<int>() > 512);
        REQUIRE(AiAssistant::detail::RequestTimeoutMs(request.config) > 8000L);
    }
}

TEST_CASE(ai_request_ignores_thinking_level_for_other_providers)
{
    // A plain OpenAI-compatible endpoint stays a plain request, thinking level or not.
    const auto request = MakeRequest("custom", "high", "https://relay.example.com/v1/chat/completions");
    const auto body = AiAssistant::detail::BuildRequestBody(request);
    REQUIRE(!body.contains("thinking"));
    REQUIRE(!body.contains("reasoning_effort"));
    REQUIRE_EQ(body["max_tokens"].get<int>(), 512);
    REQUIRE_EQ(AiAssistant::detail::RequestTimeoutMs(request.config), 8000L);
}

TEST_CASE(ai_request_follows_the_endpoint_not_the_provider_label)
{
    // 标签写着 deepseek、地址却是别的服务：带着 thinking.type 会被对方直接拒绝
    // （MiniMax 返回 2013 requires adaptive thinking），所以必须按地址判断。
    const auto foreign = MakeRequest("deepseek", "high", "https://api.minimax.cn/v1/chat/completions");
    const auto foreign_body = AiAssistant::detail::BuildRequestBody(foreign);
    REQUIRE(!foreign_body.contains("thinking"));
    REQUIRE(!foreign_body.contains("reasoning_effort"));
    REQUIRE_EQ(AiAssistant::detail::RequestTimeoutMs(foreign.config), 8000L);

    // 反过来：custom 槽指向真正的 DeepSeek 时，thinking 依然可用。
    const auto real = MakeRequest("custom", "high", "https://api.deepseek.com/chat/completions");
    const auto real_body = AiAssistant::detail::BuildRequestBody(real);
    REQUIRE_EQ(real_body["thinking"]["type"].get<std::string>(), "enabled");
    REQUIRE_EQ(real_body["reasoning_effort"].get<std::string>(), "high");
    REQUIRE(AiAssistant::detail::RequestTimeoutMs(real.config) > 8000L);
}