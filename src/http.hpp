#pragma once

#include <string>
#include <vector>

struct ChatMessage {
    std::string role;
    std::string content;
};

struct LlmResponse {
    std::string content;
    double elapsed_s = 0;
    long long tokens = 0;
    double ttft_s = 0;
};

struct ParsedChat {
    std::string content;
    long long tokens = 0;
    bool tokens_from_usage = false;
};

// Throws std::runtime_error when the body is an API error or is not a chat completion.
ParsedChat parse_chat_response(const std::string& body);

class LlmClient {
public:
    std::string api_url;
    std::string api_key;
    std::string model;
    int max_tokens = 65536;
    int timeout_s = 2400;

    LlmResponse complete(const std::vector<ChatMessage>& messages, double temperature) const;
};
