#include "utils.h"
#include <string>

std::string escapeJson(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (char c : input) {
        switch (c) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default: output += c;
        }
    }
    return output;
}

size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t totalSize = size * nmemb;
    if (output) {
        output->append(static_cast<char*>(contents), totalSize);
    }
    return totalSize;
}
