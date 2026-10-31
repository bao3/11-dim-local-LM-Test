#include "long_context.hpp"

#include "long_context_data.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

std::string build_long_context_prompt(int target_chars) {
    const std::string header = kLongHeader();
    const std::string needle1 = kNeedle1();
    const std::string needle2 = kNeedle2();
    const std::string needle3 = kNeedle3();
    const std::string footer = kLongFooter();
    const std::string fillers[] = {kFiller0(), kFiller1(), kFiller2(), kFiller3(), kFiller4()};
    constexpr int kFillerCount = 5;

    auto chars = [](const std::string& s) { return static_cast<int>(utf8_len(s)); };
    const int base_len = chars(header) + chars(needle1) + chars(needle2) + chars(needle3) + 600;
    const int remaining = std::max(3000, target_chars - base_len);
    int sum = 0;
    for (const auto& block : fillers) {
        sum += chars(block);
    }
    int avg = sum / kFillerCount;
    if (avg <= 0) {
        avg = 1;
    }
    const int total_blocks = std::max(10, remaining / avg);
    const int idx_n1 = static_cast<int>(total_blocks * 0.20);
    const int idx_n2 = static_cast<int>(total_blocks * 0.55);
    const int idx_n3 = static_cast<int>(total_blocks * 0.82);

    std::string out;
    out.reserve(static_cast<std::size_t>(std::max(target_chars, 3000)) * 4);
    out += header;
    for (int i = 0; i < total_blocks; ++i) {
        char label[80];
        std::snprintf(label, sizeof(label), "--- 节选段落 #%03d ---\n", i + 1);
        out += label;
        out += fillers[i % kFillerCount];
        if (i == idx_n1) {
            out += needle1;
        } else if (i == idx_n2) {
            out += needle2;
        } else if (i == idx_n3) {
            out += needle3;
        }
    }
    out += footer;
    return out;
}
