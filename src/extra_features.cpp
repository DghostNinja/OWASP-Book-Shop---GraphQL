#include "extra_features.h"
#include "utils.h"
#include "user_manager.h"
#include "book_manager.h"

#include <iostream>
#include <postgresql/libpq-fe.h>
#include <string>
#include <ctime>
#include <cstdlib>

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
    PGresult* res = PQexec(dbConn, "SELECT id, user_id, url, secret, is_active FROM webhooks WHERE is_active = true");

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; i++) {
            Webhook webhook;
            webhook.id = PQgetvalue(res, i, 0);
            webhook.userId = PQgetvalue(res, i, 1);
            webhook.url = PQgetvalue(res, i, 2);
            webhook.secret = PQgetvalue(res, i, 3) ? PQgetvalue(res, i, 3) : "";
            webhook.isActive = (std::string(PQgetvalue(res, i, 4)) == "t");
            webhooksCache[webhook.id] = webhook;
        }
    }
    PQclear(res);
}
