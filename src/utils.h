#ifndef UTILS_H
#define UTILS_H

#include <string>

// Function to escape JSON strings
std::string escapeJson(const std::string& input);

// Callback for CURL write function
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output);

#endif // UTILS_H
