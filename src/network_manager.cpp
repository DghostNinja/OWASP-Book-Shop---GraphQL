#include "network_manager.h"
#include <curl/curl.h>
#include <iostream>

std::vector<std::string> SSRF_WHITELIST = {
    "http://example.com",
    "http://httpbin.org",
    "http://api.github.com",
    "https://api.github.com",
    "http://169.254.169.254",
    "http://localhost:",
    "http://127.0.0.1:"
};

bool fetchURL(const std::string& url, std::string& response) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    
    return res == CURLE_OK;
}

bool isURLWhitelisted(const std::string& url) {
    for (const auto& prefix : SSRF_WHITELIST) {
        if (url.find(prefix) == 0) {
            return true;
        }
    }
    return false;
}
