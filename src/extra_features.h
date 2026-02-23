#ifndef EXTRA_FEATURES_H
#define EXTRA_FEATURES_H

#include <string>
#include <vector>
#include <map>

struct Review {
    int id;
    std::string userId;
    int bookId;
    int rating;
    std::string comment;
    bool isVerifiedPurchase;
    bool isApproved;
    std::string createdAt;
};

struct Webhook {
    std::string id;
    std::string userId;
    std::string url;
    std::string secret;
    bool isActive;
};

extern std::map<int, Review> reviewsCache;
extern std::map<std::string, Webhook> webhooksCache;

void loadReviewsCache();
void loadWebhooksCache();

#endif // EXTRA_FEATURES_H
