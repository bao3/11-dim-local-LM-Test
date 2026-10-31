#include "http.hpp"

#include "utf8.hpp"

#include <nlohmann/json.hpp>

#include <curl/curl.h>

#include <chrono>
#include <stdexcept>
#include <string>

namespace {

struct CurlGlobal {
    CurlGlobal() { curl_global_init(CURL_GLOBAL_ALL); }
    ~CurlGlobal() { curl_global_cleanup(); }
};

void ensure_curl() {
    static CurlGlobal global;
    (void)global;
}

std::size_t write_callback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string snippet(const std::string& text, std::size_t n = 400) {
    if (text.size() <= n) {
        return text;
    }
    return text.substr(0, n);
}

long long estimate_tokens(const std::string& content) {
    const auto chars = static_cast<long long>(utf8_len(content));
    return std::max<long long>(1, chars / 2);
}

}  // namespace

ParsedChat parse_chat_response(const std::string& body) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(body);
    } catch (const std::exception& ex) {
        throw std::runtime_error(std::string("response is not JSON: ") + ex.what() + " body=" + snippet(body));
    }
    if (root.contains("error") && !root.contains("choices")) {
        throw std::runtime_error("API error payload: " + snippet(body));
    }
    if (!root.contains("choices") || !root["choices"].is_array() || root["choices"].empty() ||
        !root["choices"][0].is_object() || !root["choices"][0].contains("message") ||
        !root["choices"][0]["message"].is_object()) {
        throw std::runtime_error("API response missing choices[0].message: " + snippet(body));
    }
    const auto& msg = root["choices"][0]["message"];
    std::string content;
    if (msg.contains("content") && msg["content"].is_string()) {
        content = msg["content"].get<std::string>();
    } else if (msg.contains("content") && !msg["content"].is_null()) {
        content = msg["content"].dump();
    }
    if (trim_copy(content).empty() && msg.contains("reasoning_content") && msg["reasoning_content"].is_string()) {
        content = msg["reasoning_content"].get<std::string>();
    }
    ParsedChat parsed;
    parsed.content = content;
    if (root.contains("usage") && root["usage"].is_object() && root["usage"].contains("completion_tokens") &&
        root["usage"]["completion_tokens"].is_number()) {
        parsed.tokens = root["usage"]["completion_tokens"].get<long long>();
        parsed.tokens_from_usage = true;
    } else {
        parsed.tokens = estimate_tokens(content);
        parsed.tokens_from_usage = false;
    }
    return parsed;
}

LlmResponse LlmClient::complete(const std::vector<ChatMessage>& messages, double temperature) const {
    ensure_curl();
    nlohmann::json payload;
    payload["model"] = model;
    payload["messages"] = nlohmann::json::array();
    for (const auto& message : messages) {
        payload["messages"].push_back({{"role", message.role}, {"content", message.content}});
    }
    payload["temperature"] = temperature;
    payload["stream"] = false;
    payload["max_tokens"] = max_tokens;
    payload["chat_template_kwargs"] = {{"enable_thinking", false}};
    payload["enable_thinking"] = false;
    const std::string body = payload.dump();

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        throw std::runtime_error("curl_easy_init failed");
    }
    struct CurlGuard {
        CURL* handle;
        ~CurlGuard() { curl_easy_cleanup(handle); }
    } guard{curl};

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    const std::string auth = "Authorization: Bearer " + api_key;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "Connection: close");
    headers = curl_slist_append(headers, "Accept: application/json");
    struct HeaderGuard {
        curl_slist* list;
        ~HeaderGuard() { curl_slist_free_all(list); }
    } header_guard{headers};

    curl_easy_setopt(curl, CURLOPT_URL, api_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_s));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ps5ai-11dim/1.0");

    const auto started = std::chrono::steady_clock::now();
    const CURLcode rc = curl_easy_perform(curl);
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (rc != CURLE_OK) {
        throw std::runtime_error(std::string("API Request failed: curl ") + curl_easy_strerror(rc) +
                                  " body=" + snippet(response));
    }
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code >= 400 || http_code == 0) {
        throw std::runtime_error("API Request failed: http " + std::to_string(http_code) + " body=" + snippet(response));
    }
    ParsedChat parsed = parse_chat_response(response);
    LlmResponse out;
    out.content = std::move(parsed.content);
    out.elapsed_s = elapsed;
    out.tokens = parsed.tokens;
    // Non-streaming endpoint: same TTFT approximation as the Python v3 harness.
    out.ttft_s = elapsed * 0.18;
    return out;
}
