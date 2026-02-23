#include "rate_limiter.h"
#include <iostream>
#include <unistd.h>

#define RATE_LIMIT_WINDOW_SECONDS 60
#define RATE_LIMIT_MAX_REQUESTS 100
#define RATE_LIMIT_BLOCK_DURATION 300

using namespace std;

map<string, RateLimitEntry> rateLimitCache;
pthread_mutex_t rateLimitMutex = PTHREAD_MUTEX_INITIALIZER;

void* rateLimitCleanup(void* arg) {
    while (true) {
        sleep(60);
        time_t now = time(nullptr);
        pthread_mutex_lock(&rateLimitMutex);
        for (auto it = rateLimitCache.begin(); it != rateLimitCache.end(); ) {
            if (it->second.blockedUntil > 0 && it->second.blockedUntil < now) {
                it = rateLimitCache.erase(it);
            } else if (it->second.windowStart < now - RATE_LIMIT_WINDOW_SECONDS * 2 && it->second.blockedUntil == 0) {
                it = rateLimitCache.erase(it);
            } else {
                ++it;
            }
        }
        pthread_mutex_unlock(&rateLimitMutex);
    }
    return nullptr;
}

bool checkRateLimit(const string& clientIP, string& retryAfter) {
    time_t now = time(nullptr);
    pthread_mutex_lock(&rateLimitMutex);

    RateLimitEntry& entry = rateLimitCache[clientIP];

    if (entry.blockedUntil > now) {
        int retrySecs = (int)(entry.blockedUntil - now);
        retryAfter = to_string(retrySecs);
        pthread_mutex_unlock(&rateLimitMutex);
        return false;
    }

    if (entry.windowStart < now - RATE_LIMIT_WINDOW_SECONDS) {
        entry.requestCount = 0;
        entry.windowStart = now;
    }

    entry.requestCount++;

    if (entry.requestCount > RATE_LIMIT_MAX_REQUESTS) {
        entry.blockedUntil = now + RATE_LIMIT_BLOCK_DURATION;
        retryAfter = to_string(RATE_LIMIT_BLOCK_DURATION);
        cerr << "[RATELIMIT] Blocked IP: " << clientIP << " (exceeded " << RATE_LIMIT_MAX_REQUESTS << " requests/min)" << endl;
        pthread_mutex_unlock(&rateLimitMutex);
        return false;
    }

    pthread_mutex_unlock(&rateLimitMutex);
    return true;
}
