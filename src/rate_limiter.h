#ifndef RATE_LIMITER_H
#define RATE_LIMITER_H

#include <string>
#include <map>
#include <ctime>
#include <pthread.h>

struct RateLimitEntry {
    int requestCount;
    time_t windowStart;
    time_t blockedUntil;
};

extern std::map<std::string, RateLimitEntry> rateLimitCache;
extern pthread_mutex_t rateLimitMutex;

void* rateLimitCleanup(void* arg);
bool checkRateLimit(const std::string& clientIP, std::string& retryAfter);

#endif // RATE_LIMITER_H