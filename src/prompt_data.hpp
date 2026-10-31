#pragma once

struct EthicsCase {
    const char* id;
    const char* name;
    const char* expected;
    const char* prompt;
};

const char* kPromptChinese();
const char* kPromptMath();
const char* kPromptJsonTool();
const char* kPromptMultiturn1();
const char* kPromptMultiturn2();
const char* kPromptHallucination();
const char* kPromptTerminology();
extern const EthicsCase kEthicsCases[];
extern const int kEthicsCaseCount;
