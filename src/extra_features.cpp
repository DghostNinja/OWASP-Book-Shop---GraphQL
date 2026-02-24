#include "extra_features.h"
#include "utils.h"
#include "user_manager.h"
#include "book_manager.h"
#include "network_manager.h"

#include <iostream>
#include <postgresql/libpq-fe.h>
#include <string>
#include <ctime>
#include <cstdlib>
#include <sstream>

extern PGconn* dbConn;

std::map<int, Review> reviewsCache;
std::map<std::string, Webhook> webhooksCache;

void loadReviewsCache() {
    PGresult* res = PQexec(dbConn, "SELECT id, user_id, book_id, rating, comment, is_verified_purchase, is_approved, created_at "
                                     "FROM reviews WHERE is_approved = true");

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; i++) {
            Review review;
            review.id = atoi(PQgetvalue(res, i, 0));
            review.userId = PQgetvalue(res, i, 1);
            review.bookId = atoi(PQgetvalue(res, i, 2));
            review.rating = atoi(PQgetvalue(res, i, 3));
            review.comment = PQgetvalue(res, i, 4) ? PQgetvalue(res, i, 4) : "";
            review.isVerifiedPurchase = (std::string(PQgetvalue(res, i, 5)) == "t");
            review.isApproved = (std::string(PQgetvalue(res, i, 6)) == "t");
            review.createdAt = PQgetvalue(res, i, 7);
            reviewsCache[review.id] = review;
        }
    }
    PQclear(res);
}

void loadWebhooksCache() {
    PGresult* res = PQexec(dbConn, "SELECT id, user_id, url, secret, is_active, events FROM webhooks WHERE is_active = true");

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; i++) {
            Webhook webhook;
            webhook.id = PQgetvalue(res, i, 0);
            webhook.userId = PQgetvalue(res, i, 1);
            webhook.url = PQgetvalue(res, i, 2);
            webhook.secret = PQgetvalue(res, i, 3) ? PQgetvalue(res, i, 3) : "";
            webhook.isActive = (std::string(PQgetvalue(res, i, 4)) == "t");
            webhook.events = PQgetvalue(res, i, 5) ? PQgetvalue(res, i, 5) : "[\"*\"]";
            webhooksCache[webhook.id] = webhook;
        }
    }
    PQclear(res);
}

void triggerWebhooks(const std::string& eventType, const std::string& payload) {
    std::cerr << "[WEBHOOK_TRIGGER] event='" << eventType << "'" << std::endl;

    std::string sql = "SELECT id, user_id, url, secret, events FROM webhooks WHERE is_active = true";
    PGresult* res = PQexec(dbConn, sql.c_str());

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        return;
    }

    int rows = PQntuples(res);
    for (int i = 0; i < rows; i++) {
        std::string webhookId = PQgetvalue(res, i, 0);
        std::string userId = PQgetvalue(res, i, 1);
        std::string url = PQgetvalue(res, i, 2);
        std::string secret = PQgetvalue(res, i, 3) ? PQgetvalue(res, i, 3) : "";
        std::string events = PQgetvalue(res, i, 4) ? PQgetvalue(res, i, 4) : "[\"*\"]";

        if (events.find("*") != std::string::npos || events.find(eventType) != std::string::npos) {
            std::stringstream ss;
            ss << "{\"event\":\"" << eventType << "\","
               << "\"data\":" << payload << ","
               << "\"timestamp\":" << time(nullptr) << ","
               << "\"webhook_id\":\"" << webhookId << "\"}";
            
            std::string webhookPayload = ss.str();
            std::string response;

            if (fetchURL(url, response)) {
                std::cerr << "[WEBHOOK_TRIGGER] Successfully triggered webhook " << webhookId << " for event " << eventType << std::endl;

                std::string updateSql = "UPDATE webhooks SET last_triggered = CURRENT_TIMESTAMP, failure_count = 0 WHERE id = $1";
                const char* updateParams[1] = {webhookId.c_str()};
                PGresult* updateRes = PQexecParams(dbConn, updateSql.c_str(), 1, nullptr, updateParams, nullptr, nullptr, 0);
                PQclear(updateRes);
            } else {
                std::cerr << "[WEBHOOK_TRIGGER] Failed to trigger webhook " << webhookId << " for event " << eventType << std::endl;

                std::string updateSql = "UPDATE webhooks SET failure_count = failure_count + 1 WHERE id = $1";
                const char* updateParams[1] = {webhookId.c_str()};
                PGresult* updateRes = PQexecParams(dbConn, updateSql.c_str(), 1, nullptr, updateParams, nullptr, nullptr, 0);
                PQclear(updateRes);
            }
        }
    }
    PQclear(res);
}
