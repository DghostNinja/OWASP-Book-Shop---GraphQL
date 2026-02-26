#ifndef GRAPHQL_HANDLER_H
#define GRAPHQL_HANDLER_H

#include <string>
#include <vector>
#include "user_manager.h"
#include "book_manager.h"
#include "order_manager.h"
#include "extra_features.h"
#include "utils.h"

// GraphQL Helper Functions
std::string extractJsonString(const std::string& body, size_t startPos);
std::string extractQueryFromBody(const std::string& body);
std::string extractValue(const std::string& query, const std::string& key);
std::string extractIntValue(const std::string& query, const std::string& key);
bool isFieldRequested(const std::string& query, const std::string& fieldName);
std::string extractSubQuery(const std::string& query, const std::string& fieldName);
bool isFieldRequestedInContext(const std::string& query, const std::string& contextName, const std::string& fieldName);

// JSON Conversion Functions
std::string userToJson(const User& user, const std::string& query = "");
std::string bookToJson(const Book& book, const std::string& query = "");
std::string cartItemToJson(const CartItem& item, const std::string& query = "");
std::string orderItemToJson(const OrderItem& item, const std::string& query = "");
std::string orderToJson(const Order& order, const std::string& query = "");
std::string reviewToJson(const Review& review, const std::string& query = "");
std::string webhookToJson(const Webhook& webhook, const std::string& query = "");

// GraphQL Handler Functions
std::string handleQuery(const std::string& query, const User& currentUser);
std::string handleMutation(const std::string& query, User& currentUser);

// Request Handler
User extractAuthUser(const std::string& authHeader);
AuthResult extractAuthUserWithError(const std::string& authHeader);
std::string handleRequest(const std::string& query, User& currentUser, bool isMutation);

#endif // GRAPHQL_HANDLER_H
