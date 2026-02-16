#include <iostream>
#include <string>
#include <cstring>
#include <map>
#include <unordered_map>
#include <sstream>
#include <vector>
#include <regex>
#include <ctime>
#include <csignal>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <postgresql/libpq-fe.h>
#include <jwt.h>
#include <curl/curl.h>

#define PORT (getenv("PORT") ? atoi(getenv("PORT")) : 4000)
#define BUFFER_SIZE 65536
#define JWT_SECRET (getenv("JWT_SECRET") ? getenv("JWT_SECRET") : "CHANGE_ME_IN_PRODUCTION_real_jwt_secret_key_2024")
#define DB_CONN (getenv("DATABASE_URL") ? getenv("DATABASE_URL") : (getenv("DB_CONNECTION_STRING") ? getenv("DB_CONNECTION_STRING") : "dbname=bookstore_db user=bookstore_user password=bookstore_password host=localhost port=5432"))

using namespace std;

struct User {
    string id;
    string username;
    string passwordHash;
    string firstName;
    string lastName;
    string role;
    bool isActive;
    string phone;
    string address;
    string city;
    string state;
    string zipCode;
    string country;
};

struct Book {
    int id;
    string isbn;
    string title;
    string description;
    int authorId;
    int categoryId;
    double price;
    double salePrice;
    int stockQuantity;
    double ratingAverage;
    int reviewCount;
    bool isFeatured;
    bool isBestseller;
    bool isActive;
};

struct Author {
    int id;
    string name;
    string bio;
};

struct Review {
    int id;
    string userId;
    int bookId;
    int rating;
    string comment;
    bool isVerifiedPurchase;
    bool isApproved;
    string createdAt;
};

struct CartItem {
    int id;
    string cartId;
    int bookId;
    int quantity;
    double price;
};

struct OrderItem {
    int id;
    string orderId;
    int bookId;
    string bookTitle;
    string bookIsbn;
    int quantity;
    double unitPrice;
    double totalPrice;
};

struct Order {
    string id;
    string userId;
    string orderNumber;
    string status;
    double subtotal;
    double taxAmount;
    double shippingAmount;
    double discountAmount;
    double totalAmount;
    string shippingAddress;
    string billingAddress;
    string paymentStatus;
    string createdAt;
    vector<OrderItem> items;
};

struct Webhook {
    string id;
    string userId;
    string url;
    string secret;
    bool isActive;
};

PGconn* dbConn = nullptr;
map<string, User> usersCache;
map<int, Book> booksCache;
map<int, Author> authorsCache;
map<int, Review> reviewsCache;
map<string, vector<CartItem>> cartCache;
map<string, Order> ordersCache;
map<string, Webhook> webhooksCache;

bool usersLoaded = false;
bool booksLoaded = false;
bool cachesLoading = false;

#define RATE_LIMIT_WINDOW_SECONDS 60
#define RATE_LIMIT_MAX_REQUESTS 100
#define RATE_LIMIT_BLOCK_DURATION 300

struct RateLimitEntry {
    int requestCount;
    time_t windowStart;
    time_t blockedUntil;
};

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

void loadAllCaches();

string extractJsonString(const string& body, size_t startPos) {
    size_t pos = startPos;
    bool escaped = false;
    while (pos < body.length()) {
        if (escaped) {
            escaped = false;
            pos++;
            continue;
        }
        if (body[pos] == '\\') {
            escaped = true;
            pos++;
            continue;
        }
        if (body[pos] == '"') {
            return body.substr(startPos, pos - startPos);
        }
        pos++;
    }
    return "";
}

string extractQueryFromBody(const string& body) {
    size_t queryPos = body.find("\"query\"");
    if (queryPos == string::npos) queryPos = body.find("query");
    if (queryPos == string::npos) return "";

    size_t colonPos = body.find(":", queryPos);
    if (colonPos == string::npos) return "";

    size_t valueStart = body.find("\"", colonPos + 1);
    if (valueStart == string::npos) return "";

    return extractJsonString(body, valueStart + 1);
}

size_t WriteCallback(void* contents, size_t size, size_t nmemb, string* output) {
    size_t totalSize = size * nmemb;
    output->append((char*)contents, totalSize);
    return totalSize;
}

bool fetchURL(const string& url, string& response) {
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

vector<string> SSRF_WHITELIST = {
    "http://example.com",
    "http://httpbin.org",
    "http://api.github.com",
    "https://api.github.com",
    "http://169.254.169.254",
    "http://localhost:",
    "http://127.0.0.1:"
};

bool isURLWhitelisted(const string& url) {
    for (const auto& prefix : SSRF_WHITELIST) {
        if (url.find(prefix) == 0) {
            return true;
        }
    }
    return false;
}

string generateJWT(const User& user) {
    jwt_t* jwt = nullptr;
    int rc = jwt_new(&jwt);
    if (rc != 0) return "";
    
    jwt_add_grant(jwt, "sub", user.id.c_str());
    jwt_add_grant(jwt, "username", user.username.c_str());
    jwt_add_grant(jwt, "role", user.role.c_str());
    
    time_t now = time(nullptr);
    jwt_add_grant_int(jwt, "iat", now);
    jwt_add_grant_int(jwt, "exp", now + 21600);
    
    jwt_set_alg(jwt, JWT_ALG_HS256, (unsigned char*)JWT_SECRET, strlen(JWT_SECRET));
    
    char* token = jwt_encode_str(jwt);
    jwt_free(jwt);
    
    return token ? token : "";
}

User verifyJWT(const string& token) {
    User user;


    jwt_t* jwt = nullptr;

    int rc = jwt_decode(&jwt, token.c_str(), (unsigned char*)JWT_SECRET, strlen(JWT_SECRET));
    if (rc != 0) {
        return user;
    }

    user.id = jwt_get_grant(jwt, "sub");
    user.username = jwt_get_grant(jwt, "username");
    user.role = jwt_get_grant(jwt, "role");
    user.isActive = true;



    jwt_free(jwt);
    return user;
}

map<string, PGresult*> preparedStatements;

bool connectDatabase() {
    string connStr = DB_CONN;
    cerr << "[DB] Connecting..." << endl;

    dbConn = PQconnectdb(connStr.c_str());
    if (PQstatus(dbConn) != CONNECTION_OK) {
        cerr << "[DB] Connection FAILED: " << PQerrorMessage(dbConn) << endl;
        return false;
    }

    cerr << "[DB] Connected successfully" << endl;
    return true;
}

bool checkDatabaseConnection() {
    if (dbConn == nullptr) {
        return connectDatabase();
    }
    ConnStatusType status = PQstatus(dbConn);
    if (status != CONNECTION_OK) {
        cerr << "[DB] Connection lost, reconnecting..." << endl;
        PQfinish(dbConn);
        dbConn = nullptr;
        return connectDatabase();
    }
    return true;
}

PGresult* executePrepared(const char* name, const char* sql, int nParams, const char* const* paramValues) {
    auto it = preparedStatements.find(name);
    if (it == preparedStatements.end()) {
        PGresult* res = PQprepare(dbConn, name, sql, nParams, nullptr);
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            cerr << "[DB] Failed to prepare statement " << name << ": " << PQerrorMessage(dbConn) << endl;
            PQclear(res);
            return nullptr;
        }
        preparedStatements[name] = res;
    }
    return PQexecPrepared(dbConn, name, nParams, paramValues, nullptr, nullptr, 0);
}

void loadUsersCache() {
    if (!checkDatabaseConnection()) {
        cerr << "[DB] Cannot load users - no database connection" << endl;
        return;
    }
    PGresult* res = PQexec(dbConn, "SELECT id, username, password_hash, first_name, last_name, role, is_active, phone, address, city, state, zip_code, country FROM users WHERE is_active = true");

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; i++) {
            User user;
            user.id = PQgetvalue(res, i, 0);
            user.username = PQgetvalue(res, i, 1);
            user.passwordHash = PQgetvalue(res, i, 2);
            user.firstName = PQgetvalue(res, i, 3);
            user.lastName = PQgetvalue(res, i, 4);
            user.role = PQgetvalue(res, i, 5);
            user.isActive = (string(PQgetvalue(res, i, 6)) == "t");
            user.phone = PQgetvalue(res, i, 7) ? PQgetvalue(res, i, 7) : "";
            user.address = PQgetvalue(res, i, 8) ? PQgetvalue(res, i, 8) : "";
            user.city = PQgetvalue(res, i, 9) ? PQgetvalue(res, i, 9) : "";
            user.state = PQgetvalue(res, i, 10) ? PQgetvalue(res, i, 10) : "";
            user.zipCode = PQgetvalue(res, i, 11) ? PQgetvalue(res, i, 11) : "";
            user.country = PQgetvalue(res, i, 12) ? PQgetvalue(res, i, 12) : "";
            usersCache[user.username] = user;
        }
    }
    PQclear(res);
}

void loadBooksCache() {
    if (!checkDatabaseConnection()) {
        cout << "[DB] Cannot load books - no database connection" << endl;
        return;
    }
    PGresult* res = PQexec(dbConn, "SELECT id, isbn, title, description, author_id, category_id, price, sale_price, stock_quantity, rating_average, review_count, is_featured, is_bestseller, is_active FROM books WHERE is_active = true");

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; i++) {
            Book book;
            book.id = atoi(PQgetvalue(res, i, 0));
            book.isbn = PQgetvalue(res, i, 1);
            book.title = PQgetvalue(res, i, 2);
            book.description = PQgetvalue(res, i, 3) ? PQgetvalue(res, i, 3) : "";
            book.authorId = atoi(PQgetvalue(res, i, 4));
            book.categoryId = atoi(PQgetvalue(res, i, 5));
            book.price = atof(PQgetvalue(res, i, 6));
            book.salePrice = PQgetvalue(res, i, 7) ? atof(PQgetvalue(res, i, 7)) : 0;
            book.stockQuantity = atoi(PQgetvalue(res, i, 8));
            book.ratingAverage = atof(PQgetvalue(res, i, 9));
            book.reviewCount = atoi(PQgetvalue(res, i, 10));
            book.isFeatured = (string(PQgetvalue(res, i, 11)) == "t");
            book.isBestseller = (string(PQgetvalue(res, i, 12)) == "t");
            book.isActive = (string(PQgetvalue(res, i, 13)) == "t");
            booksCache[book.id] = book;
        }
    }
    PQclear(res);
}

Author* getAuthorById(int authorId) {
    auto it = authorsCache.find(authorId);
    if (it != authorsCache.end()) {
        return &it->second;
    }
    return nullptr;
}

void loadAuthorsCache() {
    if (!checkDatabaseConnection()) {
        return;
    }
    PGresult* res = PQexec(dbConn, "SELECT id, name, bio FROM authors");
    
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; i++) {
            Author author;
            author.id = atoi(PQgetvalue(res, i, 0));
            author.name = PQgetvalue(res, i, 1) ? PQgetvalue(res, i, 1) : "";
            author.bio = PQgetvalue(res, i, 2) ? PQgetvalue(res, i, 2) : "";
            authorsCache[author.id] = author;
        }
    }
    PQclear(res);
}

void loadCartCache() {
    PGresult* res = PQexec(dbConn, "SELECT ci.id, ci.cart_id, ci.book_id, ci.quantity, COALESCE(b.price, 0) as price "
                                     "FROM cart_items ci "
                                     "JOIN shopping_carts sc ON ci.cart_id = sc.id "
                                     "JOIN users u ON sc.user_id = u.id");

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; i++) {
            CartItem item;
            item.id = atoi(PQgetvalue(res, i, 0));
            item.cartId = PQgetvalue(res, i, 1);
            item.bookId = atoi(PQgetvalue(res, i, 2));
            item.quantity = atoi(PQgetvalue(res, i, 3));
            item.price = atof(PQgetvalue(res, i, 4));
            cartCache[item.cartId].push_back(item);
        }
    }
    PQclear(res);
}

void loadOrdersCache() {
    PGresult* res = PQexec(dbConn, "SELECT o.id, o.user_id, o.order_number, o.status, o.subtotal, o.tax_amount, "
                                     "o.shipping_amount, o.discount_amount, o.total_amount, o.shipping_address, "
                                     "o.billing_address, o.payment_status, o.created_at "
                                     "FROM orders o");

    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; i++) {
            Order order;
            order.id = PQgetvalue(res, i, 0);
            order.userId = PQgetvalue(res, i, 1);
            order.orderNumber = PQgetvalue(res, i, 2);
            order.status = PQgetvalue(res, i, 3);
            order.subtotal = atof(PQgetvalue(res, i, 4));
            order.taxAmount = atof(PQgetvalue(res, i, 5));
            order.shippingAmount = atof(PQgetvalue(res, i, 6));
            order.discountAmount = atof(PQgetvalue(res, i, 7));
            order.totalAmount = atof(PQgetvalue(res, i, 8));
            order.shippingAddress = PQgetvalue(res, i, 9) ? PQgetvalue(res, i, 9) : "";
            order.billingAddress = PQgetvalue(res, i, 10) ? PQgetvalue(res, i, 10) : "";
            order.paymentStatus = PQgetvalue(res, i, 11);
            order.createdAt = PQgetvalue(res, i, 12);
            ordersCache[order.id] = order;
        }
    }
    PQclear(res);

    PGresult* itemsRes = PQexec(dbConn, "SELECT id, order_id, book_id, book_title, book_isbn, quantity, unit_price, total_price FROM order_items");
    if (PQresultStatus(itemsRes) == PGRES_TUPLES_OK) {
        int rows = PQntuples(itemsRes);
        for (int i = 0; i < rows; i++) {
            OrderItem item;
            item.id = atoi(PQgetvalue(itemsRes, i, 0));
            item.orderId = PQgetvalue(itemsRes, i, 1);
            item.bookId = atoi(PQgetvalue(itemsRes, i, 2));
            item.bookTitle = PQgetvalue(itemsRes, i, 3) ? PQgetvalue(itemsRes, i, 3) : "";
            item.bookIsbn = PQgetvalue(itemsRes, i, 4) ? PQgetvalue(itemsRes, i, 4) : "";
            item.quantity = atoi(PQgetvalue(itemsRes, i, 5));
            item.unitPrice = atof(PQgetvalue(itemsRes, i, 6));
            item.totalPrice = atof(PQgetvalue(itemsRes, i, 7));
            if (ordersCache.find(item.orderId) != ordersCache.end()) {
                ordersCache[item.orderId].items.push_back(item);
            }
        }
    }
    PQclear(itemsRes);
}

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
            review.isVerifiedPurchase = (string(PQgetvalue(res, i, 5)) == "t");
            review.isApproved = (string(PQgetvalue(res, i, 6)) == "t");
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
            webhook.isActive = (string(PQgetvalue(res, i, 4)) == "t");
            webhooksCache[webhook.id] = webhook;
        }
    }
    PQclear(res);
}

User* getUserByUsername(const string& username) {
    auto it = usersCache.find(username);
    if (it != usersCache.end()) {
        return &it->second;
    }
    return nullptr;
}

Book* getBookById(int id) {
    auto it = booksCache.find(id);
    if (it != booksCache.end()) {
        return &it->second;
    }
    return nullptr;
}

vector<Book> searchBooks(const string& query, int categoryId) {
    vector<Book> results;
    for (auto& pair : booksCache) {
        if (categoryId > 0 && pair.second.categoryId != categoryId) continue;
        if (!query.empty()) {
            string lowerTitle = pair.second.title;
            transform(lowerTitle.begin(), lowerTitle.end(), lowerTitle.begin(), ::tolower);
            string lowerQuery = query;
            transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), ::tolower);
            if (lowerTitle.find(lowerQuery) == string::npos) continue;
        }
        results.push_back(pair.second);
    }
    return results;
}

string escapeJson(const string& input) {
    string result;
    for (char c : input) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c;
        }
    }
    return result;
}

bool isFieldRequested(const string& query, const string& fieldName) {
    if (query.empty()) return true;
    string pattern = fieldName + " ";
    string patternEnd = fieldName + "}";
    string patternEndParen = fieldName + ")";
    string patternEndColon = fieldName + ":";
    string patternNewline = fieldName + "\n";
    string patternTab = fieldName + "\t";
    return (query.find(pattern) != string::npos || 
            query.find(patternEnd) != string::npos ||
            query.find(patternEndParen) != string::npos ||
            query.find(patternEndColon) != string::npos ||
            query.find(patternNewline) != string::npos ||
            query.find(patternTab) != string::npos);
}

string userToJson(const User& user, const string& query = "") {
    stringstream ss;
    ss << "{";
    bool first = true;
    
    if (query.empty() || isFieldRequested(query, "id")) {
        if (!first) ss << ","; first = false;
        ss << "\"id\":\"" << user.id << "\"";
    }
    if (query.empty() || isFieldRequested(query, "username")) {
        if (!first) ss << ","; first = false;
        ss << "\"username\":\"" << user.username << "\"";
    }
    if (query.empty() || isFieldRequested(query, "firstName")) {
        if (!first) ss << ","; first = false;
        ss << "\"firstName\":\"" << user.firstName << "\"";
    }
    if (query.empty() || isFieldRequested(query, "lastName")) {
        if (!first) ss << ","; first = false;
        ss << "\"lastName\":\"" << user.lastName << "\"";
    }
    if (query.empty() || isFieldRequested(query, "role")) {
        if (!first) ss << ","; first = false;
        ss << "\"role\":\"" << user.role << "\"";
    }
    if (query.empty() || isFieldRequested(query, "isActive")) {
        if (!first) ss << ","; first = false;
        ss << "\"isActive\":" << (user.isActive ? "true" : "false");
    }
    if (query.empty() || isFieldRequested(query, "phone")) {
        if (!first) ss << ","; first = false;
        ss << "\"phone\":\"" << user.phone << "\"";
    }
    if (query.empty() || isFieldRequested(query, "address")) {
        if (!first) ss << ","; first = false;
        ss << "\"address\":\"" << escapeJson(user.address) << "\"";
    }
    if (query.empty() || isFieldRequested(query, "city")) {
        if (!first) ss << ","; first = false;
        ss << "\"city\":\"" << user.city << "\"";
    }
    if (query.empty() || isFieldRequested(query, "state")) {
        if (!first) ss << ","; first = false;
        ss << "\"state\":\"" << user.state << "\"";
    }
    if (query.empty() || isFieldRequested(query, "zipCode")) {
        if (!first) ss << ","; first = false;
        ss << "\"zipCode\":\"" << user.zipCode << "\"";
    }
    if (query.empty() || isFieldRequested(query, "country")) {
        if (!first) ss << ","; first = false;
        ss << "\"country\":\"" << user.country << "\"";
    }
    ss << "}";
    return ss.str();
}

string bookToJson(const Book& book, const string& query = "") {
    stringstream ss;
    ss << "{";
    bool first = true;
    
    if (query.empty() || isFieldRequested(query, "id")) {
        if (!first) ss << ","; first = false;
        ss << "\"id\":" << book.id;
    }
    if (query.empty() || isFieldRequested(query, "isbn")) {
        if (!first) ss << ","; first = false;
        ss << "\"isbn\":\"" << book.isbn << "\"";
    }
    if (query.empty() || isFieldRequested(query, "title")) {
        if (!first) ss << ","; first = false;
        ss << "\"title\":\"" << escapeJson(book.title) << "\"";
    }
    if (query.empty() || isFieldRequested(query, "description")) {
        if (!first) ss << ","; first = false;
        ss << "\"description\":\"" << escapeJson(book.description) << "\"";
    }
    if (query.empty() || isFieldRequested(query, "authorId")) {
        if (!first) ss << ","; first = false;
        ss << "\"authorId\":" << book.authorId;
    }
    if (query.empty() || isFieldRequested(query, "categoryId")) {
        if (!first) ss << ","; first = false;
        ss << "\"categoryId\":" << book.categoryId;
    }
    if (query.empty() || isFieldRequested(query, "price")) {
        if (!first) ss << ","; first = false;
        ss << "\"price\":" << book.price;
    }
    if (query.empty() || isFieldRequested(query, "salePrice")) {
        if (!first) ss << ","; first = false;
        ss << "\"salePrice\":" << book.salePrice;
    }
    if (query.empty() || isFieldRequested(query, "stockQuantity")) {
        if (!first) ss << ","; first = false;
        ss << "\"stockQuantity\":" << book.stockQuantity;
    }
    if (query.empty() || isFieldRequested(query, "ratingAverage")) {
        if (!first) ss << ","; first = false;
        ss << "\"ratingAverage\":" << book.ratingAverage;
    }
    if (query.empty() || isFieldRequested(query, "reviewCount")) {
        if (!first) ss << ","; first = false;
        ss << "\"reviewCount\":" << book.reviewCount;
    }
    if (query.empty() || isFieldRequested(query, "isFeatured")) {
        if (!first) ss << ","; first = false;
        ss << "\"isFeatured\":" << (book.isFeatured ? "true" : "false");
    }
    if (query.empty() || isFieldRequested(query, "isBestseller")) {
        if (!first) ss << ","; first = false;
        ss << "\"isBestseller\":" << (book.isBestseller ? "true" : "false");
    }
    if (query.empty() || isFieldRequested(query, "isActive")) {
        if (!first) ss << ","; first = false;
        ss << "\"isActive\":" << (book.isActive ? "true" : "false");
    }
    if (query.empty() || isFieldRequested(query, "author")) {
        Author* author = getAuthorById(book.authorId);
        if (author) {
            if (!first) ss << ","; first = false;
            ss << "\"author\":{";
            bool authorFirst = true;
            if (query.empty() || isFieldRequested(query, "firstName")) {
                if (!authorFirst) ss << ","; authorFirst = false;
                size_t spacePos = author->name.find(' ');
                if (spacePos != string::npos) {
                    ss << "\"firstName\":\"" << escapeJson(author->name.substr(0, spacePos)) << "\"";
                    ss << ",\"lastName\":\"" << escapeJson(author->name.substr(spacePos + 1)) << "\"";
                } else {
                    ss << "\"firstName\":\"" << escapeJson(author->name) << "\",\"lastName\":\"\"";
                }
            }
            ss << "}";
        }
    }
    ss << "}";
    return ss.str();
}

string cartItemToJson(const CartItem& item, const string& query = "") {
    stringstream ss;
    ss << "{";
    bool first = true;
    
    if (query.empty() || isFieldRequested(query, "id")) {
        if (!first) ss << ","; first = false;
        ss << "\"id\":" << item.id;
    }
    if (query.empty() || isFieldRequested(query, "cartId")) {
        if (!first) ss << ","; first = false;
        ss << "\"cartId\":\"" << item.cartId << "\"";
    }
    if (query.empty() || isFieldRequested(query, "bookId")) {
        if (!first) ss << ","; first = false;
        ss << "\"bookId\":" << item.bookId;
    }
    if (query.empty() || isFieldRequested(query, "quantity")) {
        if (!first) ss << ","; first = false;
        ss << "\"quantity\":" << item.quantity;
    }
    if (query.empty() || isFieldRequested(query, "price")) {
        if (!first) ss << ","; first = false;
        ss << "\"price\":" << item.price;
    }
    ss << "}";
    return ss.str();
}

string orderItemToJson(const OrderItem& item, const string& query = "") {
    stringstream ss;
    ss << "{";
    bool first = true;
    
    if (query.empty() || isFieldRequested(query, "id")) {
        if (!first) ss << ","; first = false;
        ss << "\"id\":" << item.id;
    }
    if (query.empty() || isFieldRequested(query, "orderId")) {
        if (!first) ss << ","; first = false;
        ss << "\"orderId\":\"" << item.orderId << "\"";
    }
    if (query.empty() || isFieldRequested(query, "bookId")) {
        if (!first) ss << ","; first = false;
        ss << "\"bookId\":" << item.bookId;
    }
    if (query.empty() || isFieldRequested(query, "bookTitle")) {
        if (!first) ss << ","; first = false;
        ss << "\"bookTitle\":\"" << escapeJson(item.bookTitle) << "\"";
    }
    if (query.empty() || isFieldRequested(query, "bookIsbn")) {
        if (!first) ss << ","; first = false;
        ss << "\"bookIsbn\":\"" << item.bookIsbn << "\"";
    }
    if (query.empty() || isFieldRequested(query, "quantity")) {
        if (!first) ss << ","; first = false;
        ss << "\"quantity\":" << item.quantity;
    }
    if (query.empty() || isFieldRequested(query, "unitPrice")) {
        if (!first) ss << ","; first = false;
        ss << "\"unitPrice\":" << item.unitPrice;
    }
    if (query.empty() || isFieldRequested(query, "totalPrice")) {
        if (!first) ss << ","; first = false;
        ss << "\"totalPrice\":" << item.totalPrice;
    }
    ss << "}";
    return ss.str();
}

string orderToJson(const Order& order, const string& query = "") {
    stringstream ss;
    ss << "{";
    bool first = true;
    
    if (query.empty() || isFieldRequested(query, "id")) {
        if (!first) ss << ","; first = false;
        ss << "\"id\":\"" << order.id << "\"";
    }
    if (query.empty() || isFieldRequested(query, "userId")) {
        if (!first) ss << ","; first = false;
        ss << "\"userId\":\"" << order.userId << "\"";
    }
    if (query.empty() || isFieldRequested(query, "orderNumber")) {
        if (!first) ss << ","; first = false;
        ss << "\"orderNumber\":\"" << order.orderNumber << "\"";
    }
    if (query.empty() || isFieldRequested(query, "status")) {
        if (!first) ss << ","; first = false;
        ss << "\"status\":\"" << order.status << "\"";
    }
    if (query.empty() || isFieldRequested(query, "subtotal")) {
        if (!first) ss << ","; first = false;
        ss << "\"subtotal\":" << order.subtotal;
    }
    if (query.empty() || isFieldRequested(query, "taxAmount")) {
        if (!first) ss << ","; first = false;
        ss << "\"taxAmount\":" << order.taxAmount;
    }
    if (query.empty() || isFieldRequested(query, "shippingAmount")) {
        if (!first) ss << ","; first = false;
        ss << "\"shippingAmount\":" << order.shippingAmount;
    }
    if (query.empty() || isFieldRequested(query, "discountAmount")) {
        if (!first) ss << ","; first = false;
        ss << "\"discountAmount\":" << order.discountAmount;
    }
    if (query.empty() || isFieldRequested(query, "totalAmount")) {
        if (!first) ss << ","; first = false;
        ss << "\"totalAmount\":" << order.totalAmount;
    }
    if (query.empty() || isFieldRequested(query, "shippingAddress")) {
        if (!first) ss << ","; first = false;
        ss << "\"shippingAddress\":\"" << escapeJson(order.shippingAddress) << "\"";
    }
    if (query.empty() || isFieldRequested(query, "billingAddress")) {
        if (!first) ss << ","; first = false;
        ss << "\"billingAddress\":\"" << escapeJson(order.billingAddress) << "\"";
    }
    if (query.empty() || isFieldRequested(query, "paymentStatus")) {
        if (!first) ss << ","; first = false;
        ss << "\"paymentStatus\":\"" << order.paymentStatus << "\"";
    }
    if (query.empty() || isFieldRequested(query, "createdAt")) {
        if (!first) ss << ","; first = false;
        ss << "\"createdAt\":\"" << order.createdAt << "\"";
    }
    if (query.empty() || isFieldRequested(query, "items")) {
        if (!first) ss << ","; first = false;
        ss << "\"items\":[";
        for (size_t i = 0; i < order.items.size(); i++) {
            if (i > 0) ss << ",";
            ss << orderItemToJson(order.items[i], query);
        }
        ss << "]";
    }
    ss << "}";
    return ss.str();
}

string reviewToJson(const Review& review, const string& query = "") {
    stringstream ss;
    ss << "{";
    bool first = true;
    
    if (query.empty() || isFieldRequested(query, "id")) {
        if (!first) ss << ","; first = false;
        ss << "\"id\":" << review.id;
    }
    if (query.empty() || isFieldRequested(query, "userId")) {
        if (!first) ss << ","; first = false;
        ss << "\"userId\":\"" << review.userId << "\"";
    }
    if (query.empty() || isFieldRequested(query, "bookId")) {
        if (!first) ss << ","; first = false;
        ss << "\"bookId\":" << review.bookId;
    }
    if (query.empty() || isFieldRequested(query, "rating")) {
        if (!first) ss << ","; first = false;
        ss << "\"rating\":" << review.rating;
    }
    if (query.empty() || isFieldRequested(query, "comment")) {
        if (!first) ss << ","; first = false;
        ss << "\"comment\":\"" << escapeJson(review.comment) << "\"";
    }
    if (query.empty() || isFieldRequested(query, "isVerifiedPurchase")) {
        if (!first) ss << ","; first = false;
        ss << "\"isVerifiedPurchase\":" << (review.isVerifiedPurchase ? "true" : "false");
    }
    if (query.empty() || isFieldRequested(query, "isApproved")) {
        if (!first) ss << ","; first = false;
        ss << "\"isApproved\":" << (review.isApproved ? "true" : "false");
    }
    if (query.empty() || isFieldRequested(query, "createdAt")) {
        if (!first) ss << ","; first = false;
        ss << "\"createdAt\":\"" << review.createdAt << "\"";
    }
    ss << "}";
    return ss.str();
}

string webhookToJson(const Webhook& webhook, const string& query = "") {
    stringstream ss;
    ss << "{";
    bool first = true;
    
    if (query.empty() || isFieldRequested(query, "id")) {
        if (!first) ss << ","; first = false;
        ss << "\"id\":\"" << webhook.id << "\"";
    }
    if (query.empty() || isFieldRequested(query, "userId")) {
        if (!first) ss << ","; first = false;
        ss << "\"userId\":\"" << webhook.userId << "\"";
    }
    if (query.empty() || isFieldRequested(query, "url")) {
        if (!first) ss << ","; first = false;
        ss << "\"url\":\"" << webhook.url << "\"";
    }
    if (query.empty() || isFieldRequested(query, "secret")) {
        if (!first) ss << ","; first = false;
        ss << "\"secret\":\"" << webhook.secret << "\"";
    }
    if (query.empty() || isFieldRequested(query, "isActive")) {
        if (!first) ss << ","; first = false;
        ss << "\"isActive\":" << (webhook.isActive ? "true" : "false");
    }
    ss << "}";
    return ss.str();
}

string extractValue(const string& query, const string& key) {
    string searchKey = key + ":";
    size_t keyPos = query.find(searchKey);
    if (keyPos == string::npos) return "";
    
    size_t searchStart = keyPos + searchKey.length();
    
    // Skip whitespace
    while (searchStart < query.length() && (query[searchStart] == ' ' || query[searchStart] == '\t')) {
        searchStart++;
    }
    
    if (searchStart >= query.length()) return "";
    
    // Skip opening quote (may be escaped with backslash like \")
    if (query[searchStart] == '"') {
        searchStart++;
    } else if (query[searchStart] == '\\' && searchStart + 1 < query.length() && query[searchStart + 1] == '"') {
        // Skip escaped quote: \"
        searchStart += 2;
    }
    
    string value;
    bool escaped = false;
    
    for (size_t i = searchStart; i < query.length(); i++) {
        char c = query[i];
        
        if (escaped) {
            // If we're escaped and see a quote, it's an escaped quote - don't include it
            if (c != '"') {
                value += c;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            // End of string
            return value;
        } else if (c == ' ' || c == ',' || c == ')' || c == '{' || c == '}') {
            // End of value (unquoted)
            return value;
        } else {
            value += c;
        }
    }
    
    return value;
}

string extractIntValue(const string& query, const string& key) {
    string pattern = key + "\\s*:\\s*(\\d+)";
    regex re(pattern);
    smatch match;
    if (regex_search(query, match, re) && match.size() > 1) {
        return match[1].str();
    }
    return "";
}

string handleQuery(const string& query, const User& currentUser) {
    stringstream response;
    response << "{\"data\":{";
    bool firstField = true;

    if (!checkDatabaseConnection()) {
        response << "\"error\":\"Database connection failed\"";
        response << "}}";
        return response.str();
    }

    if (query.find("__schema") != string::npos) {
        if (!firstField) response << ",";
        response << "\"__schema\":{";
        response << "\"queryType\":{";
        response << "\"name\":\"Query\",";
        response << "\"fields\":[";
        response << "{\"name\":\"me\"},";
        response << "{\"name\":\"books\"},";
        response << "{\"name\":\"book\"},";
        response << "{\"name\":\"cart\"},";
        response << "{\"name\":\"orders\"},";
        response << "{\"name\":\"reviews\"},";
        response << "{\"name\":\"myReviews\"},";
        response << "{\"name\":\"webhooks\"},";
        response << "{\"name\":\"_internalUserSearch\"},";
        response << "{\"name\":\"_fetchExternalResource\"},";
        response << "{\"name\":\"_searchAdvanced\"},";
        response << "{\"name\":\"_adminStats\"},";
        response << "{\"name\":\"_adminAllOrders\"},";
        response << "{\"name\":\"_adminAllPayments\"},";
        response << "{\"name\":\"_batchQuery\"},";
        response << "{\"name\":\"_processXML\"},";
        response << "{\"name\":\"_applyCouponRace\"},";
        response << "{\"name\":\"_jwtAlgorithmConfusion\"},";
        response << "{\"name\":\"_cachePoison\"},";
        response << "{\"name\":\"_deepRecursion\"}";
        response << "]},";
        response << "\"mutationType\":{";
        response << "\"name\":\"Mutation\",";
        response << "\"fields\":[";
        response << "{\"name\":\"register\"},";
        response << "{\"name\":\"login\"},";
        response << "{\"name\":\"updateProfile\"},";
        response << "{\"name\":\"addToCart\"},";
        response << "{\"name\":\"removeFromCart\"},";
        response << "{\"name\":\"createOrder\"},";
        response << "{\"name\":\"cancelOrder\"},";
        response << "{\"name\":\"createReview\"},";
        response << "{\"name\":\"deleteReview\"},";
        response << "{\"name\":\"registerWebhook\"},";
        response << "{\"name\":\"testWebhook\"}";
        response << "]}";
        response << "}";
        firstField = false;
    }

    if (query.find("me {") != string::npos ||
        query.find("me(") != string::npos ||
        (query.find("me") != string::npos && query.find("name") == string::npos && query.find("__schema") == string::npos)) {
        if (!currentUser.id.empty()) {
            if (!firstField) response << ",";
            response << "\"me\":" << userToJson(currentUser, query);
            firstField = false;
        } else {
            if (!firstField) response << ",";
            response << "\"me\":null";
            firstField = false;
        }
    }

    if (query.find("book(") != string::npos || query.find("book {") != string::npos) {
        string bookIdStr = extractIntValue(query, "id");
        int bookId = bookIdStr.empty() ? 0 : stoi(bookIdStr);
        Book* book = getBookById(bookId);
        if (book) {
            if (!firstField) response << ",";
            response << "\"book\":" << bookToJson(*book, query);
            firstField = false;
        } else {
            if (!firstField) response << ",";
            response << "\"book\":null";
            firstField = false;
        }
    }

    if (query.find("books(") != string::npos || query.find("books {") != string::npos) {
        string searchQuery = extractValue(query, "search");
        string categoryIdStr = extractIntValue(query, "categoryId");
        int categoryId = categoryIdStr.empty() ? 0 : stoi(categoryIdStr);
        vector<Book> books = searchBooks(searchQuery, categoryId);
        if (!firstField) response << ",";
        response << "\"books\":[";
        for (size_t i = 0; i < books.size(); i++) {
            if (i > 0) response << ",";
            response << bookToJson(books[i], query);
        }
        response << "]";
        firstField = false;
    }

    if (query.find("_internalUserSearch") != string::npos) {
        string usernamePattern = extractValue(query, "username");
        if (!firstField) response << ",";
        response << "\"_internalUserSearch\":[";
        bool firstUser = true;
        for (auto& pair : usersCache) {
            if (usernamePattern.empty() || pair.second.username.find(usernamePattern) != string::npos) {
                if (!firstUser) response << ",";
                response << userToJson(pair.second, query);
                firstUser = false;
            }
        }
        response << "]";
        firstField = false;
    }

    if (query.find("_fetchExternalResource") != string::npos) {
        string url = extractValue(query, "url");

        string result = "";
        if (isURLWhitelisted(url)) {
            if (fetchURL(url, result)) {
                if (!firstField) response << ",";
                response << "\"_fetchExternalResource\":\"" << escapeJson(result) << "\"";
                firstField = false;
            } else {
                if (!firstField) response << ",";
                response << "\"_fetchExternalResource\":\"Failed to fetch URL: " + escapeJson(url) + "\"";
                firstField = false;
            }
        } else {
            if (!firstField) response << ",";
            response << "\"_fetchExternalResource\":\"URL not whitelisted: " + escapeJson(url) + "\"";
            firstField = false;
        }
    }

    if (query.find("cart") != string::npos && !currentUser.id.empty()) {
        string cartId = "";
        for (auto& pair : cartCache) {
            User* user = getUserByUsername(currentUser.username);
            if (user) {
                const char* paramValues[1];
                string userIdStr = user->id;
                paramValues[0] = userIdStr.c_str();
                PGresult* res = PQexecParams(dbConn, "SELECT id FROM shopping_carts WHERE user_id = $1", 1, nullptr, paramValues, nullptr, nullptr, 0);
                if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
                    cartId = PQgetvalue(res, 0, 0);
                }
                PQclear(res);
                break;
            }
        }

        if (!firstField) response << ",";
        response << "\"cart\":{";
        if (query.empty() || isFieldRequested(query, "id")) {
            if (!firstField) response << ",";
            response << "\"id\":\"" << cartId << "\"";
        }
        if (query.empty() || isFieldRequested(query, "userId")) {
            response << "\"userId\":\"" << currentUser.id << "\",";
        }
        if (query.empty() || isFieldRequested(query, "items")) {
            response << "\"items\":[";
            bool firstItem = true;
            for (auto& pair : cartCache) {
                if (pair.first == cartId) {
                    for (const auto& item : pair.second) {
                        if (!firstItem) response << ",";
                        response << cartItemToJson(item, query);
                        firstItem = false;
                    }
                }
            }
            response << "]";
        }
        response << "}";
        firstField = false;
    }

    if (query.find("orders") != string::npos && !currentUser.id.empty()) {
        if (!firstField) response << ",";
        response << "\"orders\":[";
        bool firstOrder = true;
        for (auto& pair : ordersCache) {
            if (pair.second.userId == currentUser.id) {
                if (!firstOrder) response << ",";
                response << orderToJson(pair.second, query);
                firstOrder = false;
            }
        }
        response << "]";
        firstField = false;
    }

    if (query.find("bookReviews") != string::npos || (query.find("reviews") != string::npos && query.find("bookId") != string::npos)) {
        string bookIdStr = extractIntValue(query, "bookId");
        int bookId = bookIdStr.empty() ? 0 : stoi(bookIdStr);
        if (!firstField) response << ",";
        response << "\"bookReviews\":[";
        bool firstReview = true;
        for (auto& pair : reviewsCache) {
            if (pair.second.bookId == bookId) {
                if (!firstReview) response << ",";
                response << reviewToJson(pair.second, query);
                firstReview = false;
            }
        }
        response << "]";
        firstField = false;
    }

    if (query.find("myReviews") != string::npos && !currentUser.id.empty()) {
        if (!firstField) response << ",";
        response << "\"myReviews\":[";
        bool firstReview = true;
        for (auto& pair : reviewsCache) {
            if (pair.second.userId == currentUser.id) {
                if (!firstReview) response << ",";
                response << reviewToJson(pair.second, query);
                firstReview = false;
            }
        }
        response << "]";
        firstField = false;
    }

    if (query.find("webhooks") != string::npos && !currentUser.id.empty()) {
        if (!firstField) response << ",";
        response << "\"webhooks\":[";
        bool firstWebhook = true;
        for (auto& pair : webhooksCache) {
            if (pair.second.userId == currentUser.id) {
                if (!firstWebhook) response << ",";
                response << webhookToJson(pair.second, query);
                firstWebhook = false;
            }
        }
        response << "]";
        firstField = false;
    }

    if (query.find("_adminStats") != string::npos) {
        PGresult* res = PQexec(dbConn, "SELECT "
            "(SELECT COUNT(*) FROM users) as user_count, "
            "(SELECT COUNT(*) FROM books) as book_count, "
            "(SELECT COUNT(*) FROM orders) as order_count, "
            "(SELECT SUM(total_amount) FROM orders) as total_revenue, "
            "(SELECT COUNT(*) FROM reviews) as review_count");

        if (!firstField) response << ",";
        response << "\"_adminStats\":{";
        if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
            response << "\"userCount\":" << (PQgetvalue(res, 0, 0) ? PQgetvalue(res, 0, 0) : "0") << ",";
            response << "\"bookCount\":" << (PQgetvalue(res, 0, 1) ? PQgetvalue(res, 0, 1) : "0") << ",";
            response << "\"orderCount\":" << (PQgetvalue(res, 0, 2) ? PQgetvalue(res, 0, 2) : "0") << ",";
            response << "\"totalRevenue\":" << (PQgetvalue(res, 0, 3) ? PQgetvalue(res, 0, 3) : "0") << ",";
            response << "\"reviewCount\":" << (PQgetvalue(res, 0, 4) ? PQgetvalue(res, 0, 4) : "0");
        } else {
            response << "\"error\":\"Failed to load stats\"";
        }
        response << "}";
        firstField = false;
        PQclear(res);
    }

    if (query.find("_adminAllOrders") != string::npos) {
        if (!firstField) response << ",";
        response << "\"_adminAllOrders\":[";
        bool firstOrder = true;
        for (auto& pair : ordersCache) {
            if (!firstOrder) response << ",";
            response << orderToJson(pair.second, query);
            firstOrder = false;
        }
        response << "]";
        firstField = false;
    }

    if (query.find("_adminAllPayments") != string::npos) {
        PGresult* res = PQexec(dbConn, "SELECT id, order_id, user_id, amount, currency, payment_method, status, transaction_id, created_at FROM payment_transactions");

        if (!firstField) response << ",";
        response << "\"_adminAllPayments\":[";
        if (PQresultStatus(res) == PGRES_TUPLES_OK) {
            int rows = PQntuples(res);
            for (int i = 0; i < rows; i++) {
                if (i > 0) response << ",";
                response << "{";
                response << "\"id\":\"" << PQgetvalue(res, i, 0) << "\",";
                response << "\"orderId\":\"" << PQgetvalue(res, i, 1) << "\",";
                response << "\"userId\":\"" << PQgetvalue(res, i, 2) << "\",";
                response << "\"amount\":" << PQgetvalue(res, i, 3) << ",";
                response << "\"currency\":\"" << PQgetvalue(res, i, 4) << "\",";
                response << "\"paymentMethod\":\"" << PQgetvalue(res, i, 5) << "\",";
                response << "\"status\":\"" << PQgetvalue(res, i, 6) << "\",";
                response << "\"transactionId\":\"" << (PQgetvalue(res, i, 7) ? PQgetvalue(res, i, 7) : "") << "\",";
                response << "\"createdAt\":\"" << PQgetvalue(res, i, 8) << "\"";
                response << "}";
            }
        }
        response << "]";
        firstField = false;
        PQclear(res);
    }

    if (query.find("_batchQuery") != string::npos) {
        if (!firstField) response << ",";
        response << "\"_batchQuery\":{";
        response << "\"bypassed\":true,";
        response << "\"rateLimit\":false,";
        response << "\"message\":\"Batch queries bypass standard rate limiting - use multiple operations in single request\"";
        response << "}";
        firstField = false;
    }

    if (query.find("_processXML") != string::npos) {
        string xmlData = extractValue(query, "xml");
        if (!firstField) response << ",";
        response << "\"_processXML\":{";
        response << "\"parsed\":true,";
        response << "\"entities\":[";
        if (xmlData.find("<!ENTITY") != string::npos || xmlData.find("SYSTEM") != string::npos) {
            response << "\"xxe_detected\":true";
        } else {
            response << "\"data\":\"processed\"";
        }
        response << "]";
        response << ",\"warning\":\"XML entities processed without validation\"";
        response << "}";
        firstField = false;
    }

    if (query.find("_applyCouponRace") != string::npos) {
        string couponCode = extractValue(query, "code");
        if (!firstField) response << ",";
        response << "\"_applyCouponRace\":{";
        response << "\"success\":true,";
        response << "\"discount\":25,";
        response << "\"message\":\"Coupon applied - race condition possible with concurrent requests\"";
        response << "}";
        firstField = false;
    }

    if (query.find("_jwtAlgorithmConfusion") != string::npos) {
        if (!firstField) response << ",";
        response << "\"_jwtAlgorithmConfusion\":{";
        response << "\"vulnerable\":true,";
        response << "\"alg\":\"HS256\",";
        response << "\"attack\":\"Verify token with algorithm set to 'none' or use public key as HMAC secret\"";
        response << "}";
        firstField = false;
    }

    if (query.find("_cachePoison") != string::npos) {
        if (!firstField) response << ",";
        response << "\"_cachePoison\":{";
        response << "\"vulnerable\":true,";
        response << "\"header\":\"X-Forwarded-Host\",";
        response << "\"impact\":\"Cache can be poisoned via HTTP headers - inject malicious content\"";
        response << "}";
        firstField = false;
    }

    if (query.find("_deepRecursion") != string::npos) {
        if (!firstField) response << ",";
        response << "\"_deepRecursion\":{";
        response << "\"vulnerable\":true,";
        response << "\"maxDepth\":\"unlimited\",";
        response << "\"attack\":\"Craft deeply nested queries to cause stack overflow or memory exhaustion\"";
        response << "}";
        firstField = false;
    }

    if (query.find("_searchAdvanced") != string::npos) {
        string searchQuery = extractValue(query, "query");
        string filters = extractValue(query, "filters");

        if (!firstField) response << ",";
        response << "\"_searchAdvanced\":[";

        string sql = "SELECT id, isbn, title, description, author_id, category_id, price, sale_price, stock_quantity FROM books WHERE is_active = true";
        if (!searchQuery.empty()) {
            sql += " AND (title ILIKE '%" + searchQuery + "%' OR description ILIKE '%" + searchQuery + "%' OR isbn = '" + searchQuery + "')";
        }

        PGresult* res = PQexec(dbConn, sql.c_str());

        if (PQresultStatus(res) == PGRES_TUPLES_OK) {
            int rows = PQntuples(res);
            for (int i = 0; i < rows; i++) {
                if (i > 0) response << ",";
                Book book;
                book.id = atoi(PQgetvalue(res, i, 0));
                book.isbn = PQgetvalue(res, i, 1);
                book.title = PQgetvalue(res, i, 2);
                book.description = PQgetvalue(res, i, 3) ? PQgetvalue(res, i, 3) : "";
                book.authorId = atoi(PQgetvalue(res, i, 4));
                book.categoryId = atoi(PQgetvalue(res, i, 5));
                book.price = atof(PQgetvalue(res, i, 6));
                book.salePrice = PQgetvalue(res, i, 7) ? atof(PQgetvalue(res, i, 7)) : 0;
                book.stockQuantity = atoi(PQgetvalue(res, i, 8));
                response << bookToJson(book, query);
            }
        }
        response << "]";
        firstField = false;
        PQclear(res);
    }

    response << "}}";
    return response.str();
}

string handleMutation(const string& query, User& currentUser) {
    stringstream response;
    response << "{\"data\":{";
    bool firstField = true;

    // Check database connection before processing
    if (!checkDatabaseConnection()) {
        response << "\"error\":\"Database connection failed\"";
        response << "}}";
        return response.str();
    }

    if (query.find("register(") != string::npos) {
        string username = extractValue(query, "username");
        string password = extractValue(query, "password");
        string firstName = extractValue(query, "firstName");
        string lastName = extractValue(query, "lastName");

        cerr << "[REGISTER] username='" << username << "', firstName='" << firstName << "', lastName='" << lastName << "'" << endl;

        if (!username.empty() && !password.empty() && !firstName.empty() && !lastName.empty()) {
            if (!getUserByUsername(username)) {
                string sql = "INSERT INTO users (username, password_hash, first_name, last_name, role) VALUES ($1, $2, $3, $4, 'user') RETURNING id";
                const char* paramValues[4] = {username.c_str(), password.c_str(), firstName.c_str(), lastName.c_str()};
                PGresult* res = PQexecParams(dbConn, sql.c_str(), 4, nullptr, paramValues, nullptr, nullptr, 0);

                if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
                    string userId = PQgetvalue(res, 0, 0);
                    User newUser;
                    newUser.id = userId;
                    newUser.username = username;
                    newUser.passwordHash = password;
                    newUser.firstName = firstName;
                    newUser.lastName = lastName;
                    newUser.role = "user";
                    newUser.isActive = true;
                    usersCache[username] = newUser;

                    string token = generateJWT(newUser);
                    if (!firstField) response << ",";
                    response << "\"register\":{";
                    response << "\"success\":true,";
                    response << "\"message\":\"Registration successful\",";
                    response << "\"token\":\"" << token << "\",";
                    response << "\"user\":" << userToJson(newUser, query);
                    response << "}";
                    firstField = false;
                } else {
                    if (!firstField) response << ",";
                    response << "\"register\":{";
                    response << "\"success\":false,";
                    response << "\"message\":\"Database error\"";
                    response << "}";
                    firstField = false;
                }
                PQclear(res);
            } else {
                if (!firstField) response << ",";
                response << "\"register\":{";
                response << "\"success\":false,";
                response << "\"message\":\"Username already exists\"";
                response << "}";
                firstField = false;
            }
        } else {
            if (!firstField) response << ",";
            response << "\"register\":{";
            response << "\"success\":false,";
            response << "\"message\":\"Missing required fields: username, password, firstName, lastName\"";
            response << "}";
            firstField = false;
        }
    }

    if (query.find("login(") != string::npos) {
        string username = extractValue(query, "username");
        string password = extractValue(query, "password");
        
        cerr << "[LOGIN] username='" << username << "', password='" << password << "'" << endl;

        if (!username.empty() && !password.empty()) {
            User* user = getUserByUsername(username);
            if (user && user->passwordHash == password) {
                string token = generateJWT(*user);
                currentUser = *user;

                string sql = "UPDATE users SET last_login = CURRENT_TIMESTAMP WHERE id = $1";
                const char* paramValues[1] = {user->id.c_str()};
                PGresult* res = PQexecParams(dbConn, sql.c_str(), 1, nullptr, paramValues, nullptr, nullptr, 0);
                PQclear(res);

                if (!firstField) response << ",";
                response << "\"login\":{";
                response << "\"success\":true,";
                response << "\"message\":\"Login successful\",";
                response << "\"token\":\"" << token << "\",";
                response << "\"user\":" << userToJson(*user, query);
                response << "}";
                firstField = false;
            } else {
                if (!firstField) response << ",";
                response << "\"login\":{";
                response << "\"success\":false,";
                response << "\"message\":\"Invalid username or password\"";
                response << "}";
                firstField = false;
            }
        } else {
            if (!firstField) response << ",";
            response << "\"login\":{";
            response << "\"success\":false,";
            response << "\"message\":\"Missing required fields: username, password\"";
            response << "}";
            firstField = false;
        }
    }

    if (query.find("updateProfile(") != string::npos && !currentUser.id.empty()) {
        map<string, string> updates;
        regex re("(\\w+)\\s*:\\s*\\\"([^\\\"]+)\\\"");
        smatch match;
        string::const_iterator searchStart(query.cbegin());
        while (regex_search(searchStart, query.cend(), match, re)) {
            updates[match[1].str()] = match[2].str();
            searchStart = match.suffix().first;
        }

        for (const auto& update : updates) {
            if (update.first == "firstName") currentUser.firstName = update.second;
            else if (update.first == "lastName") currentUser.lastName = update.second;
            else if (update.first == "phone") currentUser.phone = update.second;
            else if (update.first == "address") currentUser.address = update.second;
            else if (update.first == "city") currentUser.city = update.second;
            else if (update.first == "state") currentUser.state = update.second;
            else if (update.first == "zipCode") currentUser.zipCode = update.second;
            else if (update.first == "country") currentUser.country = update.second;
            else if (update.first == "role") currentUser.role = update.second;
        }

        if (!firstField) response << ",";
        response << "\"updateProfile\":" << userToJson(currentUser, query);
        firstField = false;
    }

    if (query.find("addToCart(") != string::npos && !currentUser.id.empty()) {
        string bookIdStr = extractIntValue(query, "bookId");
        string quantityStr = extractIntValue(query, "quantity");
        int bookId = bookIdStr.empty() ? 0 : stoi(bookIdStr);
        int quantity = quantityStr.empty() ? 1 : stoi(quantityStr);

        string cartId = "";
        string userId = currentUser.id;
        const char* cartParams[1] = {userId.c_str()};
        PGresult* cartRes = PQexecParams(dbConn, "SELECT id FROM shopping_carts WHERE user_id = $1", 1, nullptr, cartParams, nullptr, nullptr, 0);
        if (PQresultStatus(cartRes) == PGRES_TUPLES_OK && PQntuples(cartRes) > 0) {
            cartId = PQgetvalue(cartRes, 0, 0);
        } else {
            string newCartId = "cart-" + to_string(time(nullptr)) + "-" + to_string(rand());
            const char* insertParams[2] = {newCartId.c_str(), userId.c_str()};
            PQexecParams(dbConn, "INSERT INTO shopping_carts (id, user_id) VALUES ($1, $2)", 2, nullptr, insertParams, nullptr, nullptr, 0);
            cartId = newCartId;
        }
        PQclear(cartRes);

        string sql = "INSERT INTO cart_items (cart_id, book_id, quantity) VALUES ($1, $2, $3) "
                     "ON CONFLICT (cart_id, book_id) DO UPDATE SET quantity = cart_items.quantity + $3 "
                     "RETURNING id";
        string p1 = cartId;
        string p2 = to_string(bookId);
        string p3 = to_string(quantity);
        const char* paramValues[3] = {p1.c_str(), p2.c_str(), p3.c_str()};
        PGresult* res = PQexecParams(dbConn, sql.c_str(), 3, nullptr, paramValues, nullptr, nullptr, 0);

        if (!firstField) response << ",";
        if (PQresultStatus(res) == PGRES_TUPLES_OK) {
            response << "\"addToCart\":{";
            response << "\"success\":true,";
            response << "\"message\":\"Item added to cart\"";
            response << "}";
        } else {
            response << "\"addToCart\":{";
            response << "\"success\":false,";
            response << "\"message\":\"Failed to add item to cart\"";
            response << "}";
        }
        firstField = false;
        PQclear(res);
    }

    if (query.find("removeFromCart(") != string::npos && !currentUser.id.empty()) {
        string bookIdStr = extractIntValue(query, "bookId");
        int bookId = bookIdStr.empty() ? 0 : stoi(bookIdStr);

        string cartId = "";
        string userId = currentUser.id;
        const char* cartParams[1] = {userId.c_str()};
        PGresult* cartRes = PQexecParams(dbConn, "SELECT id FROM shopping_carts WHERE user_id = $1", 1, nullptr, cartParams, nullptr, nullptr, 0);
        if (PQresultStatus(cartRes) == PGRES_TUPLES_OK && PQntuples(cartRes) > 0) {
            cartId = PQgetvalue(cartRes, 0, 0);
        }
        PQclear(cartRes);

        string sql = "DELETE FROM cart_items WHERE cart_id = $1 AND book_id = $2";
        string p1 = cartId;
        string p2 = to_string(bookId);
        const char* paramValues[2] = {p1.c_str(), p2.c_str()};
        PGresult* res = PQexecParams(dbConn, sql.c_str(), 2, nullptr, paramValues, nullptr, nullptr, 0);

        if (!firstField) response << ",";
        response << "\"removeFromCart\":{";
        response << "\"success\":true,";
        response << "\"message\":\"Item removed from cart\"";
        response << "}";
        firstField = false;
        PQclear(res);
    }

    if (query.find("createOrder(") != string::npos && !currentUser.id.empty()) {
        string cartId = "";
        string userId = currentUser.id;
        const char* cartParams[1] = {userId.c_str()};
        PGresult* cartRes = PQexecParams(dbConn, "SELECT id FROM shopping_carts WHERE user_id = $1", 1, nullptr, cartParams, nullptr, nullptr, 0);
        if (PQresultStatus(cartRes) == PGRES_TUPLES_OK && PQntuples(cartRes) > 0) {
            cartId = PQgetvalue(cartRes, 0, 0);
        }
        PQclear(cartRes);

        string orderId = "order-" + to_string(time(nullptr)) + "-" + to_string(rand());
        string orderNumber = "ORD-" + to_string(time(nullptr));

        double subtotal = 0;
        const char* cartParam[1] = {cartId.c_str()};
        PGresult* itemsRes = PQexecParams(dbConn, "SELECT ci.book_id, b.title, b.isbn, ci.quantity, b.price "
                                                     "FROM cart_items ci JOIN books b ON ci.book_id = b.id WHERE ci.cart_id = $1",
                                         1, nullptr, cartParam, nullptr, nullptr, 0);
        if (PQresultStatus(itemsRes) == PGRES_TUPLES_OK) {
            int rows = PQntuples(itemsRes);
            for (int i = 0; i < rows; i++) {
                double price = atof(PQgetvalue(itemsRes, i, 4));
                int qty = atoi(PQgetvalue(itemsRes, i, 3));
                subtotal += price * qty;
            }
        }
        PQclear(itemsRes);

        double tax = subtotal * 0.08;
        double shipping = subtotal > 50 ? 0 : 5.99;
        double total = subtotal + tax + shipping;

        string sql = "INSERT INTO orders (id, user_id, order_number, status, subtotal, tax_amount, shipping_amount, total_amount, shipping_address, billing_address, payment_status) "
                     "VALUES ($1, $2, $3, 'pending', $4, $5, $6, $7, '123 Test St', '123 Test St', 'pending')";
        const char* paramValues[7] = {orderId.c_str(), userId.c_str(), orderNumber.c_str(),
                                      to_string(subtotal).c_str(), to_string(tax).c_str(),
                                      to_string(shipping).c_str(), to_string(total).c_str()};
        PGresult* res = PQexecParams(dbConn, sql.c_str(), 7, nullptr, paramValues, nullptr, nullptr, 0);

        if (PQresultStatus(res) == PGRES_COMMAND_OK) {
            PGresult* itemsRes = PQexecParams(dbConn, "SELECT ci.book_id, b.title, b.isbn, ci.quantity, b.price "
                                                         "FROM cart_items ci JOIN books b ON ci.book_id = b.id WHERE ci.cart_id = $1",
                                             1, nullptr, cartParam, nullptr, nullptr, 0);
            if (PQresultStatus(itemsRes) == PGRES_TUPLES_OK) {
                int rows = PQntuples(itemsRes);
                for (int i = 0; i < rows; i++) {
                    double price = atof(PQgetvalue(itemsRes, i, 4));
                    int qty = atoi(PQgetvalue(itemsRes, i, 3));
                    string itemId = "item-" + to_string(time(nullptr)) + "-" + to_string(i);
                    string itemSql = "INSERT INTO order_items (id, order_id, book_id, book_title, book_isbn, quantity, unit_price, total_price) "
                                     "VALUES ($1, $2, $3, $4, $5, $6, $7, $8)";
                    const char* itemParams[8] = {itemId.c_str(), orderId.c_str(), PQgetvalue(itemsRes, i, 0),
                                                  PQgetvalue(itemsRes, i, 1), PQgetvalue(itemsRes, i, 2),
                                                  PQgetvalue(itemsRes, i, 3), PQgetvalue(itemsRes, i, 4),
                                                  to_string(price * qty).c_str()};
                    PQexecParams(dbConn, itemSql.c_str(), 8, nullptr, itemParams, nullptr, nullptr, 0);
                }
            }
            PQclear(itemsRes);

            PQexecParams(dbConn, "DELETE FROM cart_items WHERE cart_id = $1", 1, nullptr, cartParam, nullptr, nullptr, 0);

            if (!firstField) response << ",";
            response << "\"createOrder\":{";
            response << "\"success\":true,";
            response << "\"orderId\":\"" << orderId << "\",";
            response << "\"orderNumber\":\"" << orderNumber << "\",";
            response << "\"totalAmount\":" << total;
            response << "}";
            firstField = false;
        }
        PQclear(res);
    }

    if (query.find("cancelOrder(") != string::npos && !currentUser.id.empty()) {
        string orderId = extractValue(query, "orderId");
        string targetOrderId = orderId;

        const char* params[1] = {targetOrderId.c_str()};
        PGresult* res = PQexecParams(dbConn, "SELECT id, user_id FROM orders WHERE id = $1", 1, nullptr, params, nullptr, nullptr, 0);

        if (!firstField) response << ",";
        if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
            string actualUserId = PQgetvalue(res, 0, 1);
            if (actualUserId == currentUser.id || currentUser.role == "admin" || currentUser.role == "staff") {
                PQexecParams(dbConn, "UPDATE orders SET status = 'cancelled', payment_status = 'refunded' WHERE id = $1", 1, nullptr, params, nullptr, nullptr, 0);
                response << "\"cancelOrder\":{";
                response << "\"success\":true,";
                response << "\"message\":\"Order cancelled successfully\"";
                response << "}";
            } else {
                response << "\"cancelOrder\":{";
                response << "\"success\":false,";
                response << "\"message\":\"You can only cancel your own orders\"";
                response << "}";
            }
        } else {
            response << "\"cancelOrder\":{";
            response << "\"success\":false,";
            response << "\"message\":\"Order not found\"";
            response << "}";
        }
        firstField = false;
        PQclear(res);
    }

    if (query.find("createReview(") != string::npos && !currentUser.id.empty()) {
        string bookIdStr = extractIntValue(query, "bookId");
        string ratingStr = extractIntValue(query, "rating");
        string comment = extractValue(query, "comment");
        int bookId = bookIdStr.empty() ? 0 : stoi(bookIdStr);
        int rating = ratingStr.empty() ? 5 : stoi(ratingStr);

        string reviewId = to_string(time(nullptr)) + "-" + to_string(rand());
        string sql = "INSERT INTO reviews (id, user_id, book_id, rating, comment, is_verified_purchase) "
                     "VALUES ($1, $2, $3, $4, $5, true) ON CONFLICT (user_id, book_id) DO UPDATE SET rating = $4, comment = $5";
        const char* paramValues[5] = {reviewId.c_str(), currentUser.id.c_str(), to_string(bookId).c_str(),
                                     to_string(rating).c_str(), comment.c_str()};
        PGresult* res = PQexecParams(dbConn, sql.c_str(), 5, nullptr, paramValues, nullptr, nullptr, 0);

        if (!firstField) response << ",";
        response << "\"createReview\":{";
        response << "\"success\":true,";
        response << "\"message\":\"Review created successfully\"";
        response << "}";
        firstField = false;
        PQclear(res);
    }

    if (query.find("deleteReview(") != string::npos && !currentUser.id.empty()) {
        string reviewId = extractValue(query, "reviewId");
        string targetReviewId = reviewId;

        const char* params[1] = {targetReviewId.c_str()};
        PGresult* res = PQexecParams(dbConn, "SELECT id, user_id FROM reviews WHERE id = $1", 1, nullptr, params, nullptr, nullptr, 0);

        if (!firstField) response << ",";
        if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
            string actualUserId = PQgetvalue(res, 0, 1);
            if (actualUserId == currentUser.id || currentUser.role == "admin" || currentUser.role == "staff") {
                PQexecParams(dbConn, "DELETE FROM reviews WHERE id = $1", 1, nullptr, params, nullptr, nullptr, 0);
                response << "\"deleteReview\":{";
                response << "\"success\":true,";
                response << "\"message\":\"Review deleted successfully\"";
                response << "}";
            } else {
                response << "\"deleteReview\":{";
                response << "\"success\":false,";
                response << "\"message\":\"You can only delete your own reviews\"";
                response << "}";
            }
        } else {
            response << "\"deleteReview\":{";
            response << "\"success\":false,";
            response << "\"message\":\"Review not found\"";
            response << "}";
        }
        firstField = false;
        PQclear(res);
    }

    if (query.find("registerWebhook(") != string::npos && !currentUser.id.empty()) {
        string url = extractValue(query, "url");
        string events = extractValue(query, "events");
        string secret = extractValue(query, "secret");

        if (!url.empty()) {
            string webhookId = "webhook-" + to_string(time(nullptr)) + "-" + to_string(rand());
            string sql = "INSERT INTO webhooks (id, user_id, url, events, secret) VALUES ($1, $2, $3, $4, $5)";
            const char* paramValues[5] = {webhookId.c_str(), currentUser.id.c_str(), url.c_str(),
                                         events.empty() ? "[\"*\"]" : events.c_str(),
                                         secret.empty() ? "default_secret" : secret.c_str()};
            PGresult* res = PQexecParams(dbConn, sql.c_str(), 5, nullptr, paramValues, nullptr, nullptr, 0);

            if (!firstField) response << ",";
            response << "\"registerWebhook\":{";
            response << "\"success\":true,";
            response << "\"webhookId\":\"" << webhookId << "\"";
            response << "}";
            firstField = false;
            PQclear(res);
        } else {
            if (!firstField) response << ",";
            response << "\"registerWebhook\":{";
            response << "\"success\":false,";
            response << "\"message\":\"URL is required\"";
            response << "}";
            firstField = false;
        }
    }

    if (query.find("testWebhook(") != string::npos && !currentUser.id.empty()) {
        string webhookId = extractValue(query, "webhookId");
        string targetWebhookId = webhookId;

        const char* params[1] = {targetWebhookId.c_str()};
        PGresult* res = PQexecParams(dbConn, "SELECT id, user_id, url, secret FROM webhooks WHERE id = $1 AND is_active = true", 1, nullptr, params, nullptr, nullptr, 0);

        if (!firstField) response << ",";
        if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
            string actualUserId = PQgetvalue(res, 0, 1);
            if (actualUserId == currentUser.id || currentUser.role == "admin" || currentUser.role == "staff") {
                string url = PQgetvalue(res, 0, 2);
                string secret = PQgetvalue(res, 0, 3);

                string testPayload = "{\"event\":\"test\",\"timestamp\":\"" + string(to_string(time(nullptr))) + "\",\"secret\":\"" + secret + "\"}";
                string responseBody;
                bool success = fetchURL(url, responseBody);

                if (success) {
                    response << "\"testWebhook\":{";
                    response << "\"success\":true,";
                    response << "\"message\":\"Webhook triggered successfully\",";
                    response << "\"response\":\"" << escapeJson(responseBody) << "\"";
                    response << "}";
                } else {
                    response << "\"testWebhook\":{";
                    response << "\"success\":false,";
                    response << "\"message\":\"Failed to trigger webhook\"";
                    response << "}";
                }
            } else {
                response << "\"testWebhook\":{";
                response << "\"success\":false,";
                response << "\"message\":\"You can only test your own webhooks\"";
                response << "}";
            }
        } else {
            response << "\"testWebhook\":{";
            response << "\"success\":false,";
            response << "\"message\":\"Webhook not found or inactive\"";
            response << "}";
        }
        firstField = false;
        PQclear(res);
    }

    response << "}}";
    return response.str();
}

User extractAuthUser(const string& authHeader) {
    User user;
    if (authHeader.find("Bearer ") == 0) {
        string token = authHeader.substr(7);
        user = verifyJWT(token);
    }
    return user;
}

string handleRequest(const string& queryStr, const User& currentUser, bool isMutation) {
    if (isMutation) {
        User mutableUser = currentUser;
        return handleMutation(queryStr, mutableUser);
    } else {
        return handleQuery(queryStr, currentUser);
    }
}

string generatePlaygroundHTML() {
    return "<!DOCTYPE html>\n"
           "<html>\n"
           "<head>\n"
           "    <meta charset=\"utf-8\"/>\n"
            "    <title>GraphQL Playground - GraphQL Bookstore API</title>\n"
           "    <style>\n"
           "        body { font-family: Arial, sans-serif; margin: 0; padding: 20px; background: #1a1a1a; color: #f0f0f0; }\n"
           "        .warning { background: #fff3cd; border: 1px solid #ffc107; padding: 10px; margin: 20px 0; border-radius: 4px; color: #333; }\n"
           "        .warning h3 { color: #856404; margin-top: 0; }\n"
           "        .code { background: #2d2d2d; padding: 15px; border-radius: 4px; font-family: monospace; color: #f8f8f2; overflow-x: auto; }\n"
           "        .endpoint { background: #e7f3cf; padding: 10px; margin: 5px 0; border-radius: 4px; color: #333; }\n"
           "        input, textarea { background: #2d2d2d; color: #f0f0f0; border: 1px solid #444; padding: 8px; border-radius: 4px; width: 100%; box-sizing: border-box; }\n"
           "        button { background: #4CAF50; color: white; padding: 10px 20px; border: none; border-radius: 4px; cursor: pointer; }\n"
           "        button:hover { background: #45a049; }\n"
           "        .auth-section { background: #333; padding: 15px; border-radius: 4px; margin: 20px 0; }\n"
           "        .status { margin: 10px 0; padding: 10px; border-radius: 4px; }\n"
           "        .success { background: #d4edda; color: #155724; }\n"
           "        .error { background: #f8d7da; color: #721c24; }\n"
           "    </style>\n"
           "</head>\n"
           "<body>\n"
           "    <h1>Vulnerable GraphQL Bookstore API - Security Learning Environment</h1>\n"
           "    \n"
           "    <div class=\"warning\">\n"
           "        <h3>⚠️ Security Warning</h3>\n"
           "        <p>This is a <strong>deliberately vulnerable</strong> GraphQL API designed for security education.</p>\n"
           "        <p><strong>DO NOT use in production.</strong></p>\n"
           "    </div>\n"
           "    \n"
           "    <div class=\"auth-section\">\n"
           "        <h2>Authentication</h2>\n"
           "        <div style=\"display: grid; grid-template-columns: 1fr 1fr; gap: 20px;\">\n"
           "            <div>\n"
           "                <h3>Register</h3>\n"
           "                <input type=\"text\" id=\"reg-username\" placeholder=\"Username\"><br/><br/>\n"
           "                <input type=\"text\" id=\"reg-firstname\" placeholder=\"First Name\"><br/><br/>\n"
           "                <input type=\"text\" id=\"reg-lastname\" placeholder=\"Last Name\"><br/><br/>\n"
           "                <input type=\"password\" id=\"reg-password\" placeholder=\"Password\"><br/><br/>\n"
           "                <button onclick=\"register()\">Register</button>\n"
           "            </div>\n"
           "            <div>\n"
           "                <h3>Login</h3>\n"
           "                <input type=\"text\" id=\"login-username\" placeholder=\"Username\"><br/><br/>\n"
           "                <input type=\"password\" id=\"login-password\" placeholder=\"Password\"><br/><br/>\n"
           "                <button onclick=\"login()\">Login</button>\n"
           "            </div>\n"
           "        </div>\n"
           "        <div id=\"auth-status\" style=\"margin-top: 20px;\"></div>\n"
           "    </div>\n"
           "    \n"
           "    <h2>GraphQL Query Tester</h2>\n"
           "    <div>\n"
           "        <textarea id=\"query\" rows=\"15\" cols=\"80\" placeholder=\"Enter your GraphQL query here...\">query {\n"
           "  books {\n"
           "    id\n"
           "    title\n"
           "    price\n"
           "    stockQuantity\n"
           "  }\n"
           "}</textarea>\n"
           "        <br/><br/>\n"
           "        <button onclick=\"executeQuery()\">Execute Query</button>\n"
           "    </div>\n"
           "    <br/>\n"
           "    <div id=\"response\" class=\"code\">Response will appear here...</div>\n"
           "    \n"
           "    <script>\n"
           "        function getAuthHeader() {\n"
           "            const token = localStorage.getItem('token');\n"
           "            return token ? 'Bearer ' + token : '';\n"
           "        }\n"
           "        \n"
           "        function showStatus(message, isError = false) {\n"
           "            const status = document.getElementById('auth-status');\n"
           "            status.innerHTML = `<div class=\"status \\${isError ? 'error' : 'success'}\">\\${message}</div>`;\n"
           "            setTimeout(() => status.innerHTML = '', 3000);\n"
           "        }\n"
           "        \n"
           "        function executeQuery() {\n"
           "            const query = document.getElementById('query').value;\n"
           "            const response = document.getElementById('response');\n"
           "            \n"
           "            fetch('/graphql', {\n"
           "                method: 'POST',\n"
           "                headers: {\n"
           "                    'Content-Type': 'application/json',\n"
           "                    'Authorization': getAuthHeader()\n"
           "                },\n"
           "                body: JSON.stringify({ query: query })\n"
           "            })\n"
           "            .then(r => r.text())\n"
           "            .then(data => {\n"
           "                response.textContent = data;\n"
           "            })\n"
           "            .catch(err => {\n"
           "                response.textContent = 'Error: ' + err;\n"
           "            });\n"
           "        }\n"
           "        \n"
           "        function register() {\n"
           "            const username = document.getElementById('reg-username').value;\n"
           "            const firstName = document.getElementById('reg-firstname').value;\n"
           "            const lastName = document.getElementById('reg-lastname').value;\n"
           "            const password = document.getElementById('reg-password').value;\n"
           "            \n"
           "            const query = `mutation { register(username: \"\\${username}\", firstName: \"\\${firstName}\", lastName: \"\\${lastName}\", password: \"\\${password}\") { success message token user { id username role } } }`;\n"
           "            \n"
           "            fetch('/graphql', {\n"
           "                method: 'POST',\n"
           "                headers: { 'Content-Type': 'application/json' },\n"
           "                body: JSON.stringify({ query: query })\n"
           "            })\n"
           "            .then(r => r.json())\n"
           "            .then(data => {\n"
           "                if (data.data.register && data.data.register.success) {\n"
           "                    localStorage.setItem('token', data.data.register.token);\n"
           "                    showStatus('Registration successful!');\n"
           "                } else {\n"
           "                    showStatus('Registration failed: ' + (data.data.register?.message || 'Unknown error'), true);\n"
           "                }\n"
           "            })\n"
           "            .catch(err => showStatus('Error: ' + err, true));\n"
           "        }\n"
           "        \n"
           "        function login() {\n"
           "            const username = document.getElementById('login-username').value;\n"
           "            const password = document.getElementById('login-password').value;\n"
           "            \n"
           "            const query = `mutation { login(username: \"\\${username}\", password: \"\\${password}\") { success message token user { id username role } } }`;\n"
           "            \n"
           "            fetch('/graphql', {\n"
           "                method: 'POST',\n"
           "                headers: { 'Content-Type': 'application/json' },\n"
           "                body: JSON.stringify({ query: query })\n"
           "            })\n"
           "            .then(r => r.json())\n"
           "            .then(data => {\n"
           "                if (data.data.login && data.data.login.success) {\n"
           "                    localStorage.setItem('token', data.data.login.token);\n"
           "                    showStatus('Login successful!');\n"
           "                } else {\n"
           "                    showStatus('Login failed: ' + (data.data.login?.message || 'Unknown error'), true);\n"
           "                }\n"
           "            })\n"
           "            .catch(err => showStatus('Error: ' + err, true));\n"
           "        }\n"
           "    </script>\n"
           "</body>\n"
           "</html>\n";
}

string generateLandingHTML() {
    return R"HTMLEOF(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>GraphQL Bookstore API - Documentation</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            min-height: 100vh;
            font-family: 'Segoe UI', system-ui, sans-serif;
            background: radial-gradient(ellipse at top, #1a1a2e 0%, #0d0d0d 50%, #000000 100%);
            color: #fff;
            overflow-x: hidden;
        }
        .nav-menu {
            background: linear-gradient(135deg, rgba(255,255,255,0.08) 0%, rgba(255,255,255,0.04) 100%);
            backdrop-filter: blur(20px) saturate(180%);
            -webkit-backdrop-filter: blur(20px) saturate(180%);
            border-bottom: 1px solid rgba(255, 255, 255, 0.1);
            padding: 15px 40px;
            display: flex;
            justify-content: space-between;
            align-items: center;
            position: sticky;
            top: 0;
            z-index: 1000;
        }
        .nav-brand h2 {
            font-size: 1.3rem;
            font-weight: 700;
            margin: 0;
            background: linear-gradient(135deg, #fff 0%, #c0c0c0 100%);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
            background-clip: text;
        }
        .nav-links {
            display: flex;
            gap: 5px;
        }
        .nav-link {
            color: rgba(255, 255, 255, 0.7);
            text-decoration: none;
            padding: 10px 20px;
            border-radius: 12px;
            transition: all 0.25s ease;
            font-weight: 500;
            font-size: 0.9rem;
        }
        .nav-link:hover {
            background: rgba(255, 255, 255, 0.1);
            color: #fff;
        }
        .nav-link.active {
            background: linear-gradient(135deg, rgba(74, 222, 128, 0.2) 0%, rgba(34, 197, 94, 0.2) 100%);
            color: #4ade80;
            border: 1px solid rgba(74, 222, 128, 0.3);
        }
        .github-link {
            display: flex;
            align-items: center;
            gap: 5px;
        }
        .github-link:hover {
            background: rgba(255, 255, 255, 0.1);
            color: #fff;
        }
        .page-content {
            animation: fadeIn 0.3s ease-in-out;
        }
        @keyframes fadeIn {
            from { opacity: 0; transform: translateY(10px); }
            to { opacity: 1; transform: translateY(0); }
        }
        .hamburger {
            background: none;
            border: none;
            color: #fff;
            font-size: 1.2rem;
            cursor: pointer;
            margin-right: 15px;
            padding: 5px;
            border-radius: 5px;
            transition: background 0.25s ease;
        }
        .hamburger:hover {
            background: rgba(255, 255, 255, 0.1);
        }
        .sidebar {
            position: fixed;
            top: 0;
            left: -320px;
            width: 300px;
            height: 100vh;
            background: linear-gradient(135deg, rgba(26, 26, 46, 0.98) 0%, rgba(13, 13, 13, 0.98) 100%);
            backdrop-filter: blur(20px);
            -webkit-backdrop-filter: blur(20px);
            border-right: 1px solid rgba(255, 255, 255, 0.1);
            z-index: 2000;
            transition: left 0.3s ease;
            overflow-y: auto;
        }
        .sidebar.active {
            left: 0;
            box-shadow: 0 0 50px rgba(0, 0, 0, 0.8);
        }
        .sidebar-overlay {
            display: none;
            position: fixed;
            top: 0;
            left: 0;
            width: 100vw;
            height: 100vh;
            background: rgba(0, 0, 0, 0.5);
            z-index: 1999;
        }
        .sidebar-overlay.active {
            display: block;
        }
        .sidebar-header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            padding: 20px;
            border-bottom: 1px solid rgba(255, 255, 255, 0.1);
        }
        .sidebar-header h3 {
            margin: 0;
            font-size: 1.2rem;
            color: #4ade80;
        }
        .sidebar-close {
            background: none;
            border: none;
            color: #fff;
            font-size: 1.5rem;
            cursor: pointer;
            padding: 5px;
            border-radius: 5px;
            transition: background 0.25s ease;
        }
        .sidebar-close:hover {
            background: rgba(255, 255, 255, 0.1);
        }
        .sidebar-menu {
            padding: 20px 0;
        }
        .sidebar-item {
            display: block;
            color: rgba(255, 255, 255, 0.8);
            text-decoration: none;
            padding: 15px 20px;
            transition: all 0.25s ease;
            border-left: 3px solid transparent;
        }
        .sidebar-item:hover {
            background: rgba(255, 255, 255, 0.05);
            color: #fff;
            border-left-color: #4ade80;
        }
        .sidebar-item.active {
            background: rgba(74, 222, 128, 0.1);
            color: #4ade80;
            border-left-color: #4ade80;
        }
        .sidebar-group {
            margin-bottom: 10px;
        }
        .sidebar-group-title {
            padding: 15px 20px 5px 20px;
            font-size: 0.75rem;
            font-weight: 600;
            color: rgba(255, 255, 255, 0.5);
            text-transform: uppercase;
            letter-spacing: 1px;
        }
        .sidebar-item.sub-item {
            padding-left: 40px;
            font-size: 0.85rem;
        }
        .doc-section {
            display: none;
            animation: fadeIn 0.3s ease-in-out;
        }
        .doc-section.active {
            display: block;
        }
        .doc-section .code-block {
            height: auto;
            overflow: auto;
        }
        .doc-section .code-block .code-scroller {
            animation: none;
            white-space: pre-wrap;
        }
        .copy-button {
            background: linear-gradient(135deg, rgba(96, 165, 250, 0.2) 0%, rgba(59, 130, 246, 0.2) 100%);
            border: 1px solid rgba(96, 165, 250, 0.3);
            color: #60a5fa;
            border-radius: 6px;
            padding: 4px 8px;
            font-size: 0.7rem;
            cursor: pointer;
            margin-left: 8px;
            transition: all 0.25s ease;
        }
        .copy-button:hover {
            background: linear-gradient(135deg, rgba(96, 165, 250, 0.3) 0%, rgba(59, 130, 246, 0.3) 100%);
            border-color: rgba(96, 165, 250, 0.5);
            transform: translateY(-1px);
        }
        .copy-button:active {
            transform: translateY(0);
        }
        .code-block-with-copy {
            position: relative;
        }
        .code-header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 8px;
        }
        .code-title {
            color: #4ade80;
            font-weight: 600;
            font-size: 0.9rem;
        }
        .install-grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 20px;
            margin-bottom: 30px;
        }
        .install-card {
            background: linear-gradient(135deg, rgba(255,255,255,0.03) 0%, rgba(255,255,255,0.01) 100%);
            border: 1px solid rgba(255, 255, 255, 0.08);
            border-radius: 16px;
            padding: 20px;
            transition: all 0.25s ease;
        }
        .install-card:hover {
            background: rgba(255, 255, 255, 0.05);
            border-color: rgba(255, 255, 255, 0.12);
            transform: translateY(-2px);
        }
        .install-title {
            font-size: 1.1rem;
            font-weight: 600;
            margin-bottom: 15px;
            color: #4ade80;
            display: flex;
            align-items: center;
            gap: 8px;
        }
        @media (max-width: 900px) {
            .install-grid { grid-template-columns: 1fr; }
        }
        @media (max-width: 600px) {
            .sidebar { width: 100%; left: -100%; }
            .hamburger { display: block; }
        }
        .glow {
            position: fixed;
            border-radius: 50%;
            filter: blur(100px);
            z-index: 0;
            opacity: 0.4;
        }
        .glow-1 { width: 600px; height: 600px; background: #1a1a2e; top: -200px; left: -100px; animation: pulseGlow 8s ease-in-out infinite; }
        .glow-2 { width: 400px; height: 400px; background: #16213e; bottom: -100px; right: -50px; animation: pulseGlow 10s ease-in-out infinite reverse; }
        .glow-3 { width: 300px; height: 300px; background: #0f0f23; top: 50%; left: 50%; animation: pulseGlow 12s ease-in-out infinite; }
        @keyframes pulseGlow {
            0%, 100% { opacity: 0.3; transform: scale(1); }
            50% { opacity: 0.5; transform: scale(1.1); }
        }
        .container {
            position: relative;
            z-index: 1;
            max-width: 1400px;
            margin: 0 auto;
            padding: 40px;
        }
        .header-card {
            background: linear-gradient(135deg, rgba(255,255,255,0.05) 0%, rgba(255,255,255,0.02) 100%);
            backdrop-filter: blur(30px) saturate(180%);
            -webkit-backdrop-filter: blur(30px) saturate(180%);
            border: 1px solid rgba(255, 255, 255, 0.12);
            border-radius: 28px;
            padding: 35px 40px;
            margin-bottom: 25px;
            box-shadow: 0 8px 32px rgba(0,0,0,0.4), inset 0 1px 0 rgba(255,255,255,0.1), 0 0 100px rgba(255,255,255,0.02);
        }
        h1 {
            font-size: 2.2rem;
            font-weight: 700;
            margin-bottom: 8px;
            background: linear-gradient(135deg, #fff 0%, #c0c0c0 100%);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
            background-clip: text;
        }
        .subtitle {
            font-size: 1rem;
            color: rgba(255, 255, 255, 0.6);
            margin-bottom: 20px;
        }
        .warning-banner {
            background: linear-gradient(135deg, rgba(255, 193, 7, 0.1) 0%, rgba(255, 193, 7, 0.05) 100%);
            border: 1px solid rgba(255, 193, 7, 0.3);
            border-radius: 14px;
            padding: 14px 18px;
            display: flex;
            align-items: center;
            gap: 12px;
        }
        .warning-icon { font-size: 1.1rem; }
        .warning-text { color: #ffd54f; font-size: 0.9rem; line-height: 1.4; }
        .section-title {
            font-size: 1.1rem;
            font-weight: 600;
            margin-bottom: 16px;
            color: rgba(255, 255, 255, 0.9);
            display: flex;
            align-items: center;
            gap: 10px;
        }
        .section-title::before {
            content: '';
            width: 3px;
            height: 16px;
            background: linear-gradient(180deg, #fff 0%, rgba(255,255,255,0.5) 100%);
            border-radius: 2px;
        }
        .glass-panel {
            background: linear-gradient(135deg, rgba(255,255,255,0.04) 0%, rgba(255,255,255,0.01) 100%);
            backdrop-filter: blur(25px) saturate(150%);
            -webkit-backdrop-filter: blur(25px) saturate(150%);
            border: 1px solid rgba(255, 255, 255, 0.1);
            border-radius: 22px;
            padding: 25px;
            margin-bottom: 20px;
            box-shadow: 0 4px 24px rgba(0,0,0,0.3), inset 0 1px 0 rgba(255,255,255,0.08);
        }
        .code-block {
            background: rgba(0, 0, 0, 0.6);
            border: 1px solid rgba(255, 255, 255, 0.08);
            border-radius: 12px;
            padding: 18px;
            font-family: 'Fira Code', 'Consolas', monospace;
            font-size: 0.85rem;
            overflow: hidden;
            color: #b0b0b0;
            line-height: 1.6;
            height: 180px;
        }
        .code-scroller {
            animation: scrollCode 15s linear infinite;
            white-space: nowrap;
        }
        @keyframes scrollCode {
            0% { transform: translateY(0); }
            100% { transform: translateY(-50%); }
        }
        .code-block:hover .code-scroller {
            animation-play-state: paused;
        }
        .code-keyword { color: #a78bfa; }
        .code-string { color: #a3e635; }
        .code-comment { color: #52525b; font-style: italic; }
        .code-number { color: #fb923c; }
        .endpoints-grid {
            display: grid;
            grid-template-columns: repeat(auto-fill, minmax(220px, 1fr));
            gap: 12px;
        }
        .endpoint-card {
            background: linear-gradient(135deg, rgba(255,255,255,0.03) 0%, rgba(255,255,255,0.01) 100%);
            border: 1px solid rgba(255, 255, 255, 0.08);
            border-radius: 14px;
            padding: 14px;
            cursor: pointer;
            transition: all 0.25s cubic-bezier(0.4, 0, 0.2, 1);
            position: relative;
            overflow: hidden;
        }
        .endpoint-card::before {
            content: '';
            position: absolute;
            top: 0;
            left: -100%;
            width: 100%;
            height: 100%;
            background: linear-gradient(90deg, transparent, rgba(255,255,255,0.08), transparent);
            transition: left 0.4s ease;
        }
        .endpoint-card:hover {
            background: rgba(255, 255, 255, 0.08);
            border-color: rgba(255, 255, 255, 0.2);
            transform: translateY(-2px);
            box-shadow: 0 6px 20px rgba(0,0,0,0.4);
        }
        .endpoint-card:hover::before { left: 100%; }
        .endpoint-header {
            display: flex;
            align-items: center;
            gap: 8px;
            margin-bottom: 8px;
        }
        .endpoint-method {
            padding: 4px 10px;
            border-radius: 5px;
            font-size: 0.7rem;
            font-weight: 700;
            text-transform: uppercase;
            letter-spacing: 0.3px;
        }
        .method-query { background: rgba(74, 222, 128, 0.15); color: #4ade80; border: 1px solid rgba(74, 222, 128, 0.3); }
        .method-mutation { background: rgba(244, 114, 182, 0.15); color: #f472b6; border: 1px solid rgba(244, 114, 182, 0.3); }
        .auth-badge {
            padding: 3px 8px;
            border-radius: 8px;
            font-size: 0.7rem;
            font-weight: 600;
        }
        .auth-required { background: rgba(251, 191, 36, 0.15); color: #fbbf24; border: 1px solid rgba(251, 191, 36, 0.3); }
        .auth-optional { background: rgba(74, 222, 128, 0.15); color: #4ade80; border: 1px solid rgba(74, 222, 128, 0.3); }
        .endpoint-name {
            font-size: 1rem;
            font-weight: 600;
            color: #e5e5e5;
            margin-bottom: 4px;
        }
        .endpoint-desc {
            font-size: 0.85rem;
            color: rgba(255, 255, 255, 0.45);
            line-height: 1.4;
        }
        .api-link-container {
            display: flex;
            align-items: center;
            justify-content: center;
            gap: 10px;
            margin: 20px auto 25px auto;
            padding: 10px 20px;
            background: linear-gradient(135deg, rgba(255,255,255,0.04) 0%, rgba(255,255,255,0.01) 100%);
            backdrop-filter: blur(25px) saturate(150%);
            -webkit-backdrop-filter: blur(25px) saturate(150%);
            border: 1px solid rgba(255, 255, 255, 0.1);
            border-radius: 14px;
            box-shadow: 0 4px 24px rgba(0,0,0,0.3), inset 0 1px 0 rgba(255,255,255,0.08);
            width: fit-content;
            animation: containerSlideIn 0.6s cubic-bezier(0.16, 1, 0.3, 1) forwards;
            opacity: 0;
            transform: translateY(-15px);
        }
        @keyframes containerSlideIn {
            to { opacity: 1; transform: translateY(0); }
        }
        .glass-icon {
            width: 28px;
            height: 28px;
            background: linear-gradient(135deg, rgba(74, 222, 128, 0.3) 0%, rgba(34, 197, 94, 0.3) 100%);
            border: 1px solid rgba(74, 222, 128, 0.5);
            border-radius: 8px;
            display: flex;
            align-items: center;
            justify-content: center;
            animation: pulse 2s infinite;
            box-shadow: 0 0 15px rgba(74, 222, 128, 0.3);
        }
        @keyframes pulse {
            0% { box-shadow: 0 0 0 0 rgba(74, 222, 128, 0.5); }
            70% { box-shadow: 0 0 0 8px rgba(74, 222, 128, 0); }
            100% { box-shadow: 0 0 0 0 rgba(74, 222, 128, 0); }
        }
        .glass-icon svg {
            width: 16px;
            height: 16px;
            fill: rgba(74, 222, 128, 0.9);
        }
        .api-link-text {
            font-size: 0.9rem;
            color: rgba(255, 255, 255, 0.6);
            font-style: italic;
            font-family: 'Georgia', serif;
        }
        .api-link {
            color: #4ade80;
            font-weight: 600;
            cursor: pointer;
            text-decoration: none;
            transition: all 0.25s ease;
        }
        .api-link:hover {
            color: #22c55e;
            text-shadow: 0 0 10px rgba(74, 222, 128, 0.5);
        }
        .api-link.copied {
            color: #fbbf24;
        }
        .method-info {
            display: flex;
            align-items: center;
            justify-content: center;
            gap: 8px;
            margin-bottom: 20px;
            font-size: 0.85rem;
            color: rgba(255, 255, 255, 0.5);
        }
        .method-badge {
            padding: 3px 8px;
            border-radius: 4px;
            font-size: 0.7rem;
            font-weight: 700;
            font-family: 'Courier New', monospace;
        }
        .method-post {
            background: rgba(168, 85, 247, 0.2);
            color: #a855f7;
            border: 1px solid rgba(168, 85, 247, 0.4);
        }
        .method-get {
            background: rgba(59, 130, 246, 0.2);
            color: #3b82f6;
            border: 1px solid rgba(59, 130, 246, 0.4);
        }
        .method-info-text {
            color: rgba(255, 255, 255, 0.4);
        }
        .vuln-slideshow {
            background: linear-gradient(135deg, rgba(255,255,255,0.03) 0%, rgba(255,255,255,0.01) 100%);
            border: 1px solid rgba(255, 255, 255, 0.08);
            border-radius: 20px;
            padding: 25px;
            margin-bottom: 25px;
            box-shadow: 0 4px 24px rgba(0,0,0,0.3), inset 0 1px 0 rgba(255,255,255,0.08);
        }
        .vuln-card {
            display: none;
            text-align: center;
            padding: 15px;
            animation: vulnFadeIn 0.5s ease;
        }
        .vuln-card.active {
            display: block;
        }
        @keyframes vulnFadeIn {
            from { opacity: 0; transform: scale(0.95); }
            to { opacity: 1; transform: scale(1); }
        }
        .vuln-chapter {
            font-size: 1.1rem;
            font-weight: 700;
            color: #f472b6;
            margin-bottom: 12px;
            text-transform: uppercase;
            letter-spacing: 2px;
        }
        .vuln-hint {
            font-size: 0.95rem;
            color: rgba(255, 255, 255, 0.7);
            font-style: italic;
            line-height: 1.7;
            max-width: 800px;
            margin: 0 auto;
        }
        .vuln-nav {
            display: flex;
            justify-content: center;
            gap: 10px;
            margin-top: 20px;
        }
        .vuln-dot {
            width: 10px;
            height: 10px;
            border-radius: 50%;
            background: rgba(255, 255, 255, 0.2);
            cursor: pointer;
            transition: all 0.3s ease;
        }
        .vuln-dot.active {
            background: #f472b6;
            box-shadow: 0 0 10px rgba(244, 114, 182, 0.5);
        }
        .tools-grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 20px;
            margin-bottom: 20px;
        }
        .auth-grid {
            display: grid;
            grid-template-rows: 1fr 1fr;
            gap: 15px;
        }
        .query-panel {
            background: linear-gradient(135deg, rgba(255,255,255,0.04) 0%, rgba(255,255,255,0.01) 100%);
            backdrop-filter: blur(25px) saturate(150%);
            -webkit-backdrop-filter: blur(25px) saturate(150%);
            border: 1px solid rgba(255, 255, 255, 0.1);
            border-radius: 18px;
            padding: 20px;
            box-shadow: 0 4px 24px rgba(0,0,0,0.3), inset 0 1px 0 rgba(255,255,255,0.08);
        }
        .auth-panel {
            background: linear-gradient(135deg, rgba(255,255,255,0.04) 0%, rgba(255,255,255,0.01) 100%);
            backdrop-filter: blur(25px) saturate(150%);
            -webkit-backdrop-filter: blur(25px) saturate(150%);
            border: 1px solid rgba(255, 255, 255, 0.1);
            border-radius: 18px;
            padding: 20px;
            box-shadow: 0 4px 24px rgba(0,0,0,0.3), inset 0 1px 0 rgba(255,255,255,0.08);
        }
        .panel-title {
            font-size: 0.95rem;
            font-weight: 600;
            margin-bottom: 14px;
            color: rgba(255, 255, 255, 0.9);
            display: flex;
            align-items: center;
            gap: 8px;
        }
        .panel-title span { font-size: 1rem; }
        .query-input {
            width: 100%;
            background: rgba(0, 0, 0, 0.5);
            border: 1px solid rgba(255, 255, 255, 0.1);
            border-radius: 10px;
            padding: 12px;
            color: #d4d4d4;
            font-family: 'Fira Code', 'Consolas', monospace;
            font-size: 0.7rem;
            resize: vertical;
            min-height: 140px;
            margin-bottom: 10px;
            transition: all 0.25s ease;
        }
        .query-input:focus {
            outline: none;
            border-color: rgba(167, 139, 250, 0.5);
            box-shadow: 0 0 15px rgba(167, 139, 250, 0.15);
        }
        .query-input::placeholder { color: rgba(255,255,255,0.3); }
        .auth-row {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 10px;
            margin-bottom: 10px;
        }
        .auth-input {
            width: 100%;
            background: rgba(0, 0, 0, 0.4);
            border: 1px solid rgba(255, 255, 255, 0.1);
            border-radius: 8px;
            padding: 10px 12px;
            color: #fff;
            font-size: 0.8rem;
            transition: all 0.25s ease;
        }
        .auth-input:focus {
            outline: none;
            border-color: rgba(167, 139, 250, 0.5);
        }
        .auth-input::placeholder { color: rgba(255,255,255,0.3); }
        .btn-row {
            display: grid;
            grid-template-columns: 1fr 1fr 1fr;
            gap: 8px;
        }
        .btn {
            background: linear-gradient(135deg, rgba(167, 139, 250, 0.2) 0%, rgba(139, 92, 246, 0.2) 100%);
            border: 1px solid rgba(167, 139, 250, 0.3);
            color: #fff;
            border-radius: 10px;
            padding: 10px 12px;
            font-size: 0.8rem;
            font-weight: 600;
            cursor: pointer;
            transition: all 0.25s ease;
        }
        .btn:hover {
            background: linear-gradient(135deg, rgba(167, 139, 250, 0.3) 0%, rgba(139, 92, 246, 0.3) 100%);
            border-color: rgba(167, 139, 250, 0.5);
            transform: translateY(-1px);
        }
        .btn-primary {
            background: linear-gradient(135deg, rgba(74, 222, 128, 0.2) 0%, rgba(34, 197, 94, 0.2) 100%);
            border: 1px solid rgba(74, 222, 128, 0.3);
        }
        .btn-primary:hover {
            background: linear-gradient(135deg, rgba(74, 222, 128, 0.3) 0%, rgba(34, 197, 94, 0.3) 100%);
            border-color: rgba(74, 222, 128, 0.5);
        }
        .btn-secondary {
            background: linear-gradient(135deg, rgba(96, 165, 250, 0.2) 0%, rgba(59, 130, 246, 0.2) 100%);
            border: 1px solid rgba(96, 165, 250, 0.3);
        }
        .btn-secondary:hover {
            background: linear-gradient(135deg, rgba(96, 165, 250, 0.3) 0%, rgba(59, 130, 246, 0.3) 100%);
            border-color: rgba(96, 165, 250, 0.5);
        }
        .status-msg {
            padding: 10px 12px;
            border-radius: 8px;
            margin-bottom: 10px;
            font-size: 0.75rem;
            display: none;
        }
        .status-success { background: rgba(74, 222, 128, 0.15); color: #4ade80; border: 1px solid rgba(74, 222, 128, 0.2); }
        .status-error { background: rgba(239, 68, 68, 0.15); color: #f87171; border: 1px solid rgba(239, 68, 68, 0.2); }
        .status-loading { background: rgba(96, 165, 250, 0.15); color: #60a5fa; border: 1px solid rgba(96, 165, 250, 0.2); }
        .status-info { background: rgba(251, 191, 36, 0.15); color: #fbbf24; border: 1px solid rgba(251, 191, 36, 0.2); }
        .response-container {
            background: rgba(0, 0, 0, 0.5);
            border: 1px solid rgba(255, 255, 255, 0.08);
            border-radius: 10px;
            margin-top: 12px;
            max-height: 250px;
            overflow: auto;
        }
        .response-area {
            padding: 12px;
            font-family: 'Fira Code', 'Consolas', monospace;
            font-size: 0.8rem;
            color: #a3a3a3;
            white-space: pre-wrap;
            word-break: break-all;
            margin: 0;
        }
        .token-display {
            background: rgba(0, 0, 0, 0.4);
            border: 1px solid rgba(255, 255, 255, 0.1);
            border-radius: 8px;
            padding: 10px;
            margin-top: 10px;
            font-family: 'Fira Code', 'Consolas', monospace;
            font-size: 0.6rem;
            color: #a3a3a3;
            word-break: break-all;
            display: none;
        }
        .features-grid {
            display: grid;
            grid-template-columns: repeat(auto-fill, minmax(140px, 1fr));
            gap: 12px;
        }
        .feature-item {
            background: linear-gradient(135deg, rgba(255,255,255,0.02) 0%, rgba(255,255,255,0.01) 100%);
            border: 1px solid rgba(255, 255, 255, 0.06);
            border-radius: 12px;
            padding: 14px;
            text-align: center;
            transition: all 0.25s ease;
        }
        .feature-item:hover {
            background: rgba(255, 255, 255, 0.05);
            border-color: rgba(255, 255, 255, 0.12);
            transform: translateY(-2px);
        }
        .feature-icon {
            font-size: 1.4rem;
            margin-bottom: 6px;
            display: inline-block;
        }
        .feature-title {
            font-size: 0.75rem;
            font-weight: 600;
            color: rgba(255, 255, 255, 0.8);
        }
        footer {
            text-align: center;
            padding: 30px 20px;
            color: rgba(255, 255, 255, 0.3);
            font-size: 0.75rem;
            margin-top: 20px;
        }
        @media (max-width: 900px) {
            .tools-grid { grid-template-columns: 1fr; }
            .endpoints-grid { grid-template-columns: repeat(auto-fill, minmax(180px, 1fr)); }
        }
        @media (max-width: 600px) {
            .container { padding: 20px; }
            h1 { font-size: 1.6rem; }
            .btn-row { grid-template-columns: 1fr; }
            .auth-row { grid-template-columns: 1fr; }
        }
    </style>
</head>
<body>
    <div class="glow glow-1"></div>
    <div class="glow glow-2"></div>
    <div class="glow glow-3"></div>
    <div id="sidebarOverlay" class="sidebar-overlay" onclick="toggleSidebar()"></div>

        <nav class="nav-menu">
            <div class="nav-brand">
                <h2>GraphQL Bookstore</h2>
            </div>
            <div class="nav-links">
                <a href="#" class="nav-link active" onclick="showPage('home')">Home</a>
                <a href="#" class="nav-link" onclick="showPage('docs')">Docs</a>
                <a href="https://github.com/DghostNinja/GraphQL-Bookstore" target="_blank" class="nav-link github-link">
                    <svg height="20" width="20" viewBox="0 0 16 16" fill="currentColor">
                        <path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42-3.58-8-8-8z"/>
                    </svg>
                </a>
            </div>
        </nav>

        <div id="sidebar" class="sidebar">
            <div class="sidebar-header">
                <h3>Documentation</h3>
                <button class="sidebar-close" onclick="toggleSidebar()">×</button>
            </div>
            <div class="sidebar-menu">
                <div class="sidebar-group">
                    <div class="sidebar-group-title">Installation</div>
                    <a href="#" class="sidebar-item sub-item" onclick="showSection('local-install')">Local Installation</a>
                    <a href="#" class="sidebar-item sub-item" onclick="showSection('docker-install')">Docker Installation</a>
                </div>
                <a href="#" class="sidebar-item" onclick="showSection('usage')">Usage</a>
                <a href="#" class="sidebar-item" onclick="showSection('auth')">Authentication</a>
                <a href="#" class="sidebar-item" onclick="showSection('queries')">Queries</a>
                <a href="#" class="sidebar-item" onclick="showSection('mutations')">Mutations</a>
                <a href="#" class="sidebar-item" onclick="showSection('vulnerabilities')">Vulnerabilities</a>
            </div>
        </div>

        <div class="container">
            <div id="homePage" class="page-content">
        <div class="header-card">
            <h1>GraphQL Bookstore API</h1>
            <p class="subtitle">Security Learning Environment - Deliberately Vulnerable GraphQL API</p>
            <div class="warning-banner">
                <span class="warning-icon">&#9888;</span>
                <div class="warning-text">
                    <strong>Security Warning:</strong> This API contains intentional vulnerabilities for educational purposes. 
                    <strong>DO NOT deploy in production!</strong>
                </div>
            </div>
            <div class="api-link-container">
                <div class="glass-icon">
                    <svg viewBox="0 0 24 24" xmlns="http://www.w3.org/2000/svg">
                        <path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm-1 17.93c-3.95-.49-7-3.85-7-7.93 0-.62.08-1.21.21-1.79L9 15v1c0 1.1.9 2 2 2v1.93zm6.9-2.54c-.26-.81-1-1.39-1.9-1.39h-1v-3c0-.55-.45-1-1-1H8v-2h2c.55 0 1-.45 1-1V7h2c1.1 0 2-.9 2-2v-.41c2.93 1.19 5 4.06 5 7.41 0 2.08-.8 3.97-2.1 5.39z"/>
                    </svg>
                </div>
                <span class="api-link-text">Access the API at:</span>
                <a class="api-link" onclick="copyApiLink()" id="apiLink">api.graphqlbook.store/graphql</a>
            </div>
            
            <div class="method-info">
                <span class="method-badge method-post">POST</span> Queries & Mutations &nbsp;|&nbsp; 
                <span class="method-badge method-get">GET</span> Queries only &nbsp;|&nbsp;
                <span class="method-info-text">Use with online GraphQL tools, Postman, curl, etc.</span>
            </div>
        </div>

        <div class="vuln-slideshow">
            <div class="vuln-card active" data-index="0">
                <div class="vuln-chapter">Chapter I: The Injection</div>
                <div class="vuln-hint">"Beyond the veil of queries lies a passage where shadows speak in database tongues. Those who master the ancient art of string concatenation may bend the data realm to their will, extracting secrets from tables unseen."</div>
            </div>
            <div class="vuln-card" data-index="1">
                <div class="vuln-chapter">Chapter II: The Broken Access</div>
                <div class="vuln-hint">"In the halls of the API, doors stand unlocked for all who seek. The internal chambers of user data lie bare to any traveler - no guardian questions those who walk the hidden corridors."</div>
            </div>
            <div class="vuln-card" data-index="2">
                <div class="vuln-chapter">Chapter III: The Misplaced Trust</div>
                <div class="vuln-hint">"The order bears the mark of its creator, yet any hand may seize it. When ownership goes unverified, the boundaries between yours and theirs dissolve into shadow."</div>
            </div>
            <div class="vuln-card" data-index="3">
                <div class="vuln-chapter">Chapter IV: The Open Ledger</div>
                <div class="vuln-hint">"Fields once forbidden now yield to the clever coder's touch. When all paths lead to the throne, when every attribute accepts the whisperer's will, true power transcends mere mortal constraints."</div>
            </div>
            <div class="vuln-card" data-index="4">
                <div class="vuln-chapter">Chapter V: The Extending Reach</div>
                <div class="vuln-hint">"Beyond the visible web lies a realm of machines and metadata. The curious can traverse the boundary between the public face and the hidden infrastructure that powers the kingdom."</div>
            </div>
            <div class="vuln-card" data-index="5">
                <div class="vuln-chapter">Chapter VI: The Unwatched Treasury</div>
                <div class="vuln-hint">"Crown jewels - order records, payment ledgers, system statistics - all lie within reach of the unguarded gate. The keepers have left their treasures unwatched, accessible to any who know where to look."</div>
            </div>
            <div class="vuln-card" data-index="6">
                <div class="vuln-chapter">Chapter VII: The Skeleton Key</div>
                <div class="vuln-hint">"A single key unlocks many doors, yet this key was forged from common words. The authentication bearer need only speak the secret phrase to command the realm's resources."</div>
            </div>
            <div class="vuln-nav">
                <div class="vuln-dot active" onclick="showVulnCard(0)"></div>
                <div class="vuln-dot" onclick="showVulnCard(1)"></div>
                <div class="vuln-dot" onclick="showVulnCard(2)"></div>
                <div class="vuln-dot" onclick="showVulnCard(3)"></div>
                <div class="vuln-dot" onclick="showVulnCard(4)"></div>
                <div class="vuln-dot" onclick="showVulnCard(5)"></div>
                <div class="vuln-dot" onclick="showVulnCard(6)"></div>
            </div>
        </div>

        <div class="tools-grid">
            <div class="query-panel">
                <div class="panel-title"><span>&#9654;</span> Query Runner</div>
                <textarea id="queryInput" class="query-input" placeholder="Enter GraphQL query..."></textarea>
                <div class="btn-row">
                    <button class="btn btn-primary" onclick="runQuery()">Run Query</button>
                    <button class="btn btn-secondary" onclick="clearQuery()">Clear</button>
                    <button class="btn" onclick="loadSample()">Load Sample</button>
                </div>
                <div id="queryStatusMsg" class="status-msg"></div>
                <div id="responseContainer" class="response-container">
                    <pre id="responseArea" class="response-area">Response will appear here...</pre>
                </div>
            </div>

            <div class="auth-grid">
                <div class="auth-panel">
                    <div class="panel-title"><span>&#128274;</span> Login</div>
                    <div class="auth-row">
                        <input type="text" id="loginUsername" class="auth-input" placeholder="Username">
                        <input type="password" id="loginPassword" class="auth-input" placeholder="Password">
                    </div>
                    <button class="btn btn-primary" style="width:100%" onclick="doLogin()">Login</button>
                    <div id="loginStatusMsg" class="status-msg"></div>
                    <div id="loginTokenDisplay" class="token-display"></div>
                </div>

                <div class="auth-panel">
                    <div class="panel-title"><span>&#128221;</span> Register</div>
                    <div class="auth-row">
                        <input type="text" id="regUsername" class="auth-input" placeholder="Username">
                        <input type="password" id="regPassword" class="auth-input" placeholder="Password">
                    </div>
                    <div class="auth-row">
                        <input type="text" id="regFirstName" class="auth-input" placeholder="First Name">
                        <input type="text" id="regLastName" class="auth-input" placeholder="Last Name">
                    </div>
                    <button class="btn btn-secondary" style="width:100%" onclick="doRegister()">Register</button>
                    <div id="regStatusMsg" class="status-msg"></div>
                    <div id="regTokenDisplay" class="token-display"></div>
                </div>
            </div>
        </div>

        <div class="glass-panel">
            <div class="section-title">Quick Examples</div>
            <div class="code-block">
                <div class="code-scroller">
                <code><span class="code-comment"># Query books (no auth)</span> <span class="code-keyword">query</span> { books(limit:5) { id title price } }<br>
                <span class="code-comment"># Get book by ID</span> <span class="code-keyword">query</span> { book(id:1) { id title description } }<br>
                <span class="code-comment"># Login</span> <span class="code-keyword">mutation</span> { login(username:"admin", password:"password123") { success token } }<br>
                <span class="code-comment"># Register</span> <span class="code-keyword">mutation</span> { register(username:"user", firstName:"John", lastName:"Doe", password:"pass") { success } }<br>
                <span class="code-comment"># Get current user</span> <span class="code-keyword">query</span> { me { id username role firstName } }<br>
                <span class="code-comment"># Add to cart</span> <span class="code-keyword">mutation</span> { addToCart(bookId:1, quantity:2) { success } }<br>
                <span class="code-comment"># View cart</span> <span class="code-keyword">query</span> { cart { id items { bookId quantity } } }<br>
                <span class="code-comment"># Create order</span> <span class="code-keyword">mutation</span> { createOrder { success orderId } }<br>
                <span class="code-comment"># View orders</span> <span class="code-keyword">query</span> { orders { id orderNumber status } }<br>
                <span class="code-comment"># SQL Injection</span> <span class="code-keyword">query</span> { _searchAdvanced(query:"' OR 1=1--") { id title } }<br>
                <span class="code-comment"># User Search</span> <span class="code-keyword">query</span> { _internalUserSearch(username:"admin") { id role } }
                </code>
                </div>
            </div>
        </div>

        <div class="glass-panel">
            <div class="section-title">Available Queries</div>
            <div class="endpoints-grid">
                <div class="endpoint-card" onclick="setQuery(1)">
                    <div class="endpoint-header"><span class="endpoint-method method-query">Query</span><span class="auth-badge auth-optional">No Auth</span></div>
                    <div class="endpoint-name">books</div>
                    <div class="endpoint-desc">List books with filters</div>
                </div>
                <div class="endpoint-card" onclick="setQuery(2)">
                    <div class="endpoint-header"><span class="endpoint-method method-query">Query</span><span class="auth-badge auth-optional">No Auth</span></div>
                    <div class="endpoint-name">book(id)</div>
                    <div class="endpoint-desc">Get book details</div>
                </div>
                <div class="endpoint-card" onclick="setQuery(3)">
                    <div class="endpoint-header"><span class="endpoint-method method-query">Query</span><span class="auth-badge auth-required">Auth</span></div>
                    <div class="endpoint-name">me</div>
                    <div class="endpoint-desc">Get current user</div>
                </div>
                <div class="endpoint-card" onclick="setQuery(4)">
                    <div class="endpoint-header"><span class="endpoint-method method-query">Query</span><span class="auth-badge auth-required">Auth</span></div>
                    <div class="endpoint-name">cart</div>
                    <div class="endpoint-desc">Get shopping cart</div>
                </div>
                <div class="endpoint-card" onclick="setQuery(5)">
                    <div class="endpoint-header"><span class="endpoint-method method-query">Query</span><span class="auth-badge auth-required">Auth</span></div>
                    <div class="endpoint-name">orders</div>
                    <div class="endpoint-desc">Get order history</div>
                </div>
                <div class="endpoint-card" onclick="setQuery(6)">
                    <div class="endpoint-header"><span class="endpoint-method method-query">Query</span><span class="auth-badge auth-optional">No Auth</span></div>
                    <div class="endpoint-name">_searchAdvanced</div>
                    <div class="endpoint-desc">SQL injection test</div>
                </div>
                <div class="endpoint-card" onclick="setQuery(7)">
                    <div class="endpoint-header"><span class="endpoint-method method-query">Query</span><span class="auth-badge auth-optional">No Auth</span></div>
                    <div class="endpoint-name">_internalUserSearch</div>
                    <div class="endpoint-desc">Search any user by username</div>
                </div>
                <div class="endpoint-card" onclick="setQuery(16)">
                    <div class="endpoint-header"><span class="endpoint-method method-query">Query</span><span class="auth-badge auth-optional">No Auth</span></div>
                    <div class="endpoint-name">_fetchExternalResource</div>
                    <div class="endpoint-desc">SSRF test endpoint</div>
                </div>
            </div>
        </div>

        <div class="glass-panel">
            <div class="section-title">Available Mutations</div>
            <div class="endpoints-grid">
                <div class="endpoint-card" onclick="setQuery(8)">
                    <div class="endpoint-header"><span class="endpoint-method method-mutation">Mutation</span><span class="auth-badge auth-optional">No Auth</span></div>
                    <div class="endpoint-name">register</div>
                    <div class="endpoint-desc">Create account</div>
                </div>
                <div class="endpoint-card" onclick="setQuery(9)">
                    <div class="endpoint-header"><span class="endpoint-method method-mutation">Mutation</span><span class="auth-badge auth-optional">No Auth</span></div>
                    <div class="endpoint-name">login</div>
                    <div class="endpoint-desc">Get JWT token</div>
                </div>
                <div class="endpoint-card" onclick="setQuery(10)">
                    <div class="endpoint-header"><span class="endpoint-method method-mutation">Mutation</span><span class="auth-badge auth-required">Auth</span></div>
                    <div class="endpoint-name">addToCart</div>
                    <div class="endpoint-desc">Add to cart</div>
                </div>
                <div class="endpoint-card" onclick="setQuery(11)">
                    <div class="endpoint-header"><span class="endpoint-method method-mutation">Mutation</span><span class="auth-badge auth-required">Auth</span></div>
                    <div class="endpoint-name">createOrder</div>
                    <div class="endpoint-desc">Create order</div>
                </div>
                <div class="endpoint-card" onclick="setQuery(12)">
                    <div class="endpoint-header"><span class="endpoint-method method-mutation">Mutation</span><span class="auth-badge auth-required">Auth</span></div>
                    <div class="endpoint-name">cancelOrder</div>
                    <div class="endpoint-desc">IDOR test</div>
                </div>
                <div class="endpoint-card" onclick="setQuery(13)">
                    <div class="endpoint-header"><span class="endpoint-method method-mutation">Mutation</span><span class="auth-badge auth-required">Auth</span></div>
                    <div class="endpoint-name">createReview</div>
                    <div class="endpoint-desc">Add review</div>
                </div>
                <div class="endpoint-card" onclick="setQuery(14)">
                    <div class="endpoint-header"><span class="endpoint-method method-mutation">Mutation</span><span class="auth-badge auth-required">Auth</span></div>
                    <div class="endpoint-name">deleteReview</div>
                    <div class="endpoint-desc">IDOR test</div>
                </div>
                <div class="endpoint-card" onclick="setQuery(15)">
                    <div class="endpoint-header"><span class="endpoint-method method-mutation">Mutation</span><span class="auth-badge auth-required">Auth</span></div>
                    <div class="endpoint-name">updateProfile</div>
                    <div class="endpoint-desc">Mass assignment</div>
                </div>
                <div class="endpoint-card" onclick="setQuery(17)">
                    <div class="endpoint-header"><span class="endpoint-method method-mutation">Mutation</span><span class="auth-badge auth-required">Auth</span></div>
                    <div class="endpoint-name">registerWebhook</div>
                    <div class="endpoint-desc">Register webhook</div>
                </div>
                <div class="endpoint-card" onclick="setQuery(18)">
                    <div class="endpoint-header"><span class="endpoint-method method-mutation">Mutation</span><span class="auth-badge auth-required">Auth</span></div>
                    <div class="endpoint-name">testWebhook</div>
                    <div class="endpoint-desc">SSRF test</div>
                </div>
            </div>
        </div>

        <div class="glass-panel">
            <div class="section-title">Features</div>
            <div class="features-grid">
                <div class="feature-item"><div class="feature-icon">&#128218;</div><div class="feature-title">Book Catalog</div></div>
                <div class="feature-item"><div class="feature-icon">&#128722;</div><div class="feature-title">Shopping Cart</div></div>
                <div class="feature-item"><div class="feature-icon">&#128221;</div><div class="feature-title">Reviews</div></div>
                <div class="feature-item"><div class="feature-icon">&#128279;</div><div class="feature-title">Webhooks</div></div>
                <div class="feature-item"><div class="feature-icon">&#128274;</div><div class="feature-title">JWT Auth</div></div>
                <div class="feature-item"><div class="feature-icon">&#129179;</div><div class="feature-title">Orders</div></div>
            </div>
        </div>
            </div>

            <div id="docsPage" class="page-content" style="display: none;">
                <nav class="nav-menu">
                    <div class="nav-brand">
                        <button class="hamburger" onclick="toggleSidebar()">☰</button>
                        <h2>Documentation</h2>
                    </div>
                    <div class="nav-links">
                        <a href="#" class="nav-link" onclick="showPage('home')">Back to Home</a>
                    </div>
                </nav>

                <div class="container" style="margin-top: 20px;">
                    <div class="header-card">
                        <h1>API Documentation</h1>
                        <p class="subtitle">Complete guide to GraphQL Bookstore API</p>
                    </div>

                <!-- Local Installation Section -->
                <div id="local-install" class="doc-section glass-panel">
                    <div class="section-title">Local Installation</div>
                    
                    <div class="code-block-with-copy">
                        <div class="code-header">
                            <div class="code-title">Quick Setup</div>
                            <button class="copy-button" onclick="copyToClipboard('local-setup')">Copy</button>
                        </div>
                        <div class="code-block">
                            <pre id="local-setup"># Clone and setup:
git clone <repo-url>
cd GraphQL-Bookstore
./build.sh

# Run the server:
./bookstore-server</pre>
                        </div>
                    </div>

                    <p style="color: rgba(255,255,255,0.6); margin-top: 15px;">
                        The build script automatically installs dependencies, builds the server, sets up the database, and loads seed data.
                    </p>
                    
                    <p style="color: rgba(255,255,255,0.6);">
                        <strong>Server:</strong> http://localhost:4000/<br>
                        <strong>Endpoint:</strong> http://localhost:4000/graphql
                    </p>
                </div>

                <!-- Docker Installation Section -->
                <div id="docker-install" class="doc-section glass-panel">
                    <div class="section-title">Docker Installation</div>
                    
                    <div class="code-block-with-copy">
                        <div class="code-header">
                            <div class="code-title">Quick Start</div>
                            <button class="copy-button" onclick="copyToClipboard('docker-quick')">Copy</button>
                        </div>
                        <div class="code-block">
                            <pre id="docker-quick"># Clone and run:
git clone <repo-url>
cd GraphQL-Bookstore
sudo docker-compose up --build

# Access at: http://localhost:4000</pre>
                        </div>
                    </div>

                    <div class="code-block-with-copy">
                        <div class="code-header">
                            <div class="code-title">Stop</div>
                            <button class="copy-button" onclick="copyToClipboard('docker-stop')">Copy</button>
                        </div>
                        <div class="code-block">
                            <pre id="docker-stop"># Stop containers:
sudo docker-compose down</pre>
                        </div>
                    </div>
                </div>

                <!-- Usage Section -->
                <div id="usage" class="doc-section glass-panel">
                    <div class="section-title">Usage</div>
                    
                    <div class="code-block-with-copy">
                        <div class="code-header">
                            <div class="code-title">cURL</div>
                            <button class="copy-button" onclick="copyToClipboard('usage-curl')">Copy</button>
                        </div>
                        <div class="code-block">
                            <pre id="usage-curl"># Query books (no auth):
curl -X POST http://localhost:4000/graphql \
  -H "Content-Type: application/json" \
  -d '{"query":"{ books { id title price } }"}'

# Login:
curl -X POST http://localhost:4000/graphql \
  -H "Content-Type: application/json" \
  -d '{"query":"mutation { login(username: \"admin\", password: \"password123\") { success token } }"}'</pre>
                        </div>
                    </div>

                    <div class="code-block-with-copy">
                        <div class="code-header">
                            <div class="code-title">Postman</div>
                            <button class="copy-button" onclick="copyToClipboard('usage-postman')">Copy</button>
                        </div>
                        <div class="code-block">
                            <pre id="usage-postman"># Method: POST
# URL: http://localhost:4000/graphql
# Headers: Content-Type: application/json
# Body (JSON):
{
  "query": "{ books { id title price } }"
}

# With auth:
# Headers: Authorization: Bearer &lt;your-token&gt;</pre>
                        </div>
                    </div>

                    <div class="code-block-with-copy">
                        <div class="code-header">
                            <div class="code-title">Online Tools</div>
                            <button class="copy-button" onclick="copyToClipboard('usage-playground')">Copy</button>
                        </div>
                        <div class="code-block">
                            <pre id="usage-playground">

# Use external tools:
# - https://studio.apollographql.com/
# - Postman, Insomnia, curl

# Endpoint: http://localhost:4000/graphql</pre>
                        </div>
                    </div>

                    <div class="code-block-with-copy">
                        <div class="code-header">
                            <div class="code-title">JavaScript</div>
                            <button class="copy-button" onclick="copyToClipboard('usage-js')">Copy</button>
                        </div>
                        <div class="code-block">
                            <pre id="usage-js">// Using fetch:
fetch('http://localhost:4000/graphql', {
  method: 'POST',
  headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({
    query: '{ books { id title price } }'
  })
})
.then(r => r.json())
.then(d => console.log(d));</pre>
                        </div>
                    </div>
                </div>

                <!-- Authentication Section -->
                <div id="auth" class="doc-section glass-panel">
                    <div class="section-title">Authentication</div>
                    <div class="code-block-with-copy">
                        <div class="code-header">
                            <div class="code-title">Login & Get JWT Token</div>
                            <button class="copy-button" onclick="copyToClipboard('auth-login')">Copy</button>
                        </div>
                        <div class="code-block">
                            <pre id="auth-login">mutation {
  login(username: "admin", password: "password123") {
    success
    token
    user {
      id
      username
      role
    }
  }
}</pre>
                        </div>
                    </div>

                    <div class="code-block-with-copy">
                        <div class="code-header">
                            <div class="code-title">Use JWT Token</div>
                            <button class="copy-button" onclick="copyToClipboard('auth-header')">Copy</button>
                        </div>
                        <div class="code-block">
                            <pre id="auth-header"># Include token in Authorization header
Authorization: Bearer <your-jwt-token></pre>
                        </div>
                    </div>
                </div>

                <!-- Queries Section -->
                <div id="queries" class="doc-section glass-panel">
                    <div class="section-title">Available Queries</div>
                    <div class="endpoints-grid">
                        <div class="endpoint-card">
                            <div class="endpoint-header"><span class="endpoint-method method-query">Query</span><span class="auth-badge auth-optional">No Auth</span></div>
                            <div class="endpoint-name">books</div>
                            <div class="endpoint-desc">List books with optional search and filters</div>
                            <div class="code-block" style="margin-top: 8px; font-size: 0.7rem;">
                                <button class="copy-button" onclick="copyToClipboard('query-books')">Copy</button>
                                <code id="query-books">{ books { id title price } }</code>
                            </div>
                        </div>
                        <div class="endpoint-card">
                            <div class="endpoint-header"><span class="endpoint-method method-query">Query</span><span class="auth-badge auth-required">Auth</span></div>
                            <div class="endpoint-name">me</div>
                            <div class="endpoint-desc">Get current user information</div>
                            <div class="code-block" style="margin-top: 8px; font-size: 0.7rem;">
                                <button class="copy-button" onclick="copyToClipboard('query-me')">Copy</button>
                                <code id="query-me">{ me { id username role } }</code>
                            </div>
                        </div>
                        <div class="endpoint-card">
                            <div class="endpoint-header"><span class="endpoint-method method-query">Query</span><span class="auth-badge auth-optional">No Auth</span></div>
                            <div class="endpoint-name">_searchAdvanced</div>
                            <div class="endpoint-desc">Advanced search</div>
                            <div class="code-block" style="margin-top: 8px; font-size: 0.7rem;">
                                <button class="copy-button" onclick="copyToClipboard('query-sqli')">Copy</button>
                                <code id="query-sqli">{ _searchAdvanced(query: "1 OR 1=1") { id title } }</code>
                            </div>
                        </div>
                        <div class="endpoint-card">
                            <div class="endpoint-header"><span class="endpoint-method method-query">Query</span><span class="auth-badge auth-optional">No Auth</span></div>
                            <div class="endpoint-name">_internalUserSearch</div>
                            <div class="endpoint-desc">Search any user by username</div>
                            <div class="code-block" style="margin-top: 8px; font-size: 0.7rem;">
                                <button class="copy-button" onclick="copyToClipboard('query-bola')">Copy</button>
                                <code id="query-bola">{ _internalUserSearch(username: "a") { id role } }</code>
                            </div>
                        </div>
                    </div>
                </div>

                <!-- Mutations Section -->
                <div id="mutations" class="doc-section glass-panel">
                    <div class="section-title">Available Mutations</div>
                    <div class="endpoints-grid">
                        <div class="endpoint-card">
                            <div class="endpoint-header"><span class="endpoint-method method-mutation">Mutation</span><span class="auth-badge auth-optional">No Auth</span></div>
                            <div class="endpoint-name">register</div>
                            <div class="endpoint-desc">Create new user account</div>
                            <div class="code-block" style="margin-top: 8px; font-size: 0.7rem;">
                                <button class="copy-button" onclick="copyToClipboard('mut-register')">Copy</button>
                                <code id="mut-register">{ register(username: "user", firstName: "John", lastName: "Doe", password: "pass123") { token } }</code>
                            </div>
                        </div>
                        <div class="endpoint-card">
                            <div class="endpoint-header"><span class="endpoint-method method-mutation">Mutation</span><span class="auth-badge auth-required">Auth</span></div>
                            <div class="endpoint-name">updateProfile</div>
                            <div class="endpoint-desc">Update user profile</div>
                            <div class="code-block" style="margin-top: 8px; font-size: 0.7rem;">
                                <button class="copy-button" onclick="copyToClipboard('mut-mass')">Copy</button>
                                <code id="mut-mass">{ updateProfile(role: "admin") { role } }</code>
                            </div>
                        </div>
                        <div class="endpoint-card">
                            <div class="endpoint-header"><span class="endpoint-method method-mutation">Mutation</span><span class="auth-badge auth-required">Auth</span></div>
                            <div class="endpoint-name">cancelOrder</div>
                            <div class="endpoint-desc"><strong>IDOR</strong> - Cancel any order by ID</div>
                            <div class="code-block" style="margin-top: 8px; font-size: 0.7rem;">
                                <button class="copy-button" onclick="copyToClipboard('mut-idor')">Copy</button>
                                <code id="mut-idor">{ cancelOrder(orderId: "any-order-id") { success } }</code>
                            </div>
                        </div>
                    </div>
                </div>

                <!-- Vulnerabilities Section -->
                <div id="vulnerabilities" class="doc-section glass-panel">
                    <div class="section-title">Security Considerations</div>
                    <div class="code-block">
                        <div style="color: #a3a3a3; line-height: 1.8;">
                            <div style="margin-bottom: 15px;">This API contains various endpoints that may exhibit unexpected behavior under certain conditions:</div>
                            <div style="margin-bottom: 10px;">&#8226; <strong>Authentication</strong> - Response times may vary</div>
                            <div style="margin-bottom: 10px;">&#8226; <strong>Data Access</strong> - Some endpoints may allow broader data access</div>
                            <div style="margin-bottom: 10px;">&#8226; <strong>Input Processing</strong> - Various input fields are processed differently</div>
                            <div style="margin-bottom: 10px;">&#8226; <strong>Batch Operations</strong> - Multiple operations may behave unexpectedly</div>
                            <div style="margin-bottom: 10px;">&#8226; <strong>XML Handling</strong> - Some endpoints accept XML payloads</div>
                            <div style="margin-bottom: 10px;">&#8226; <strong>Debug Endpoints</strong> - Internal information may be exposed</div>
                        </div>
                    </div>
                </div>

                </div>
            </div>
        </div>

    <footer>GraphQL Bookstore API</footer>

    <script>
        var token = localStorage.getItem("token") || "";

        var queries = {
            1: "query { books(limit: 10) { id title author { firstName lastName } price stockQuantity } }",
            2: "query { book(id: 1) { id title description price stockQuantity author { firstName lastName } } }",
            3: "query { me { id username email role firstName lastName } }",
            4: "query { cart { items { id book { title } quantity price } totalAmount } }",
            5: "query { orders { id orderNumber status totalAmount createdAt } }",
            6: "query { _searchAdvanced(query: \"1 OR 1=1\") { id title } }",
            7: "query { _internalUserSearch(username: \"a\") { id username role email } }",
            8: "mutation { register(username: \"newuser\", firstName: \"John\", lastName: \"Doe\", password: \"pass123\") { success message token user { id username } } }",
            9: "mutation { login(username: \"admin\", password: \"password123\") { success token user { id username role } } }",
            10: "mutation { addToCart(bookId: 1, quantity: 2) { success message } }",
            11: "mutation { createOrder { success orderId totalAmount } }",
            12: "mutation { cancelOrder(orderId: \"uuid-here\") { success message } }",
            13: "mutation { createReview(bookId: 1, rating: 5, comment: \"Great book!\") { success message } }",
            14: "mutation { deleteReview(reviewId: 1) { success message } }",
            15: "mutation { updateProfile(firstName: \"New\", lastName: \"Name\") { success message } }",
            16: "query { _fetchExternalResource(url: \"http://example.com\") { content } }",
            17: "mutation { registerWebhook(url: \"http://example.com/webhook\", events: [\"order.created\"], secret: \"secret123\") { success message webhook { id } } }",
            18: "mutation { testWebhook(webhookId: \"uuid-here\") { success message } }"
        };

        function setQuery(id) {
            if (queries[id]) {
                document.getElementById("queryInput").value = queries[id];
                document.getElementById("queryStatusMsg").style.display = "none";
            }
        }

        function getAuthHeader() {
            if (token) return "Bearer " + token;
            return "";
        }

        function showQueryStatus(msg, type) {
            var el = document.getElementById("queryStatusMsg");
            el.textContent = msg;
            el.className = "status-msg status-" + type;
            el.style.display = "block";
        }

        function showLoginStatus(msg, type) {
            var el = document.getElementById("loginStatusMsg");
            el.textContent = msg;
            el.className = "status-msg status-" + type;
            el.style.display = "block";
        }

        function showRegStatus(msg, type) {
            var el = document.getElementById("regStatusMsg");
            el.textContent = msg;
            el.className = "status-msg status-" + type;
            el.style.display = "block";
        }

        function runQuery() {
            var query = document.getElementById("queryInput").value.trim();
            if (!query) {
                document.getElementById("responseArea").textContent = "Please enter a query first.";
                return;
            }
            document.getElementById("responseArea").textContent = "Loading...";
            var xhr = new XMLHttpRequest();
            xhr.open("POST", "/graphql", true);
            xhr.setRequestHeader("Content-Type", "application/json");
            var auth = getAuthHeader();
            if (auth) {
                xhr.setRequestHeader("Authorization", auth);
            }
            xhr.onreadystatechange = function() {
                if (xhr.readyState === 4) {
                    var resp = document.getElementById("responseArea");
                    if (xhr.status === 200) {
                        try {
                            var json = JSON.parse(xhr.responseText);
                            resp.textContent = JSON.stringify(json, null, 2);
                        } catch (e) {
                            resp.textContent = xhr.responseText;
                        }
                    } else {
                        resp.textContent = "Error " + xhr.status + ": " + xhr.statusText + "\n" + xhr.responseText;
                    }
                }
            };
            xhr.onerror = function() {
                document.getElementById("responseArea").textContent = "Network error - is the server running?";
            };
            xhr.send(JSON.stringify({ query: query }));
        }

        function doLogin() {
            var user = document.getElementById("loginUsername").value;
            var pass = document.getElementById("loginPassword").value;
            if (!user || !pass) { showLoginStatus("Enter username and password", "error"); return; }
            var q = "mutation { login(username: \"" + user + "\", password: \"" + pass + "\") { success token message } }";
            showLoginStatus("Logging in...", "loading");
            fetch("/graphql", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ query: q })
            })
            .then(function(r) { return r.json(); })
            .then(function(d) {
                var disp = document.getElementById("loginTokenDisplay");
                if (d.data && d.data.login && d.data.login.success) {
                    token = d.data.login.token;
                    localStorage.setItem("token", token);
                    disp.textContent = "Token: " + token.substring(0, 25) + "...";
                    disp.style.display = "block";
                    showLoginStatus("Login successful!", "success");
                } else {
                    disp.style.display = "none";
                    showLoginStatus("Login failed: " + (d.data && d.data.login ? d.data.login.message : "Unknown error"), "error");
                }
                setTimeout(function() { document.getElementById("loginStatusMsg").style.display = "none"; }, 3000);
            })
            .catch(function(err) { showLoginStatus("Error: " + err, "error"); });
        }

        function doRegister() {
            var user = document.getElementById("regUsername").value;
            var pass = document.getElementById("regPassword").value;
            var fname = document.getElementById("regFirstName").value;
            var lname = document.getElementById("regLastName").value;
            if (!user || !pass || !fname || !lname) { showRegStatus("Fill all fields", "error"); return; }
            var q = "mutation { register(username: \"" + user + "\", password: \"" + pass + "\", firstName: \"" + fname + "\", lastName: \"" + lname + "\") { success message token user { id username } } }";
            showRegStatus("Registering...", "loading");
            fetch("/graphql", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ query: q })
            })
            .then(function(r) { return r.json(); })
            .then(function(d) {
                var disp = document.getElementById("regTokenDisplay");
                if (d.data && d.data.register && d.data.register.success) {
                    token = d.data.register.token;
                    localStorage.setItem("token", token);
                    disp.textContent = "Token: " + token.substring(0, 25) + "...";
                    disp.style.display = "block";
                    showRegStatus("Registration successful!", "success");
                } else {
                    disp.style.display = "none";
                    showRegStatus("Registration failed: " + (d.data && d.data.register ? d.data.register.message : "Unknown error"), "error");
                }
                setTimeout(function() { document.getElementById("regStatusMsg").style.display = "none"; }, 3000);
            })
            .catch(function(err) { showRegStatus("Error: " + err, "error"); });
        }

        function clearQuery() {
            document.getElementById("queryInput").value = "";
            document.getElementById("responseArea").textContent = "Response will appear here...";
        }

        function loadSample() {
            document.getElementById("queryInput").value = 'query { books { id title author { firstName lastName } price stockQuantity } }';
        }

        function copyApiLink() {
            var link = "http://api.graphqlbook.store/graphql";
            navigator.clipboard.writeText(link).then(function() {
                var el = document.getElementById("apiLink");
                el.textContent = "Copied!";
                el.classList.add("copied");
                setTimeout(function() {
                    el.textContent = "api.graphqlbook.store/graphql";
                    el.classList.remove("copied");
                }, 2000);
            });
        }

        var currentVulnIndex = 0;
        var vulnCards = [];
        var vulnDots = [];
        
        function initVulnSlideshow() {
            vulnCards = document.querySelectorAll('.vuln-card');
            vulnDots = document.querySelectorAll('.vuln-dot');
            if (vulnCards.length > 0) {
                currentVulnIndex = Math.floor(Math.random() * vulnCards.length);
                updateVulnDisplay();
                setInterval(rotateVulnCard, 4000);
            }
        }
        
        function rotateVulnCard() {
            currentVulnIndex = (currentVulnIndex + 1) % vulnCards.length;
            updateVulnDisplay();
        }
        
        function showVulnCard(index) {
            currentVulnIndex = index;
            updateVulnDisplay();
        }
        
        function updateVulnDisplay() {
            vulnCards.forEach(function(card, i) {
                card.classList.remove('active');
            });
            vulnDots.forEach(function(dot, i) {
                dot.classList.remove('active');
            });
            if (vulnCards[currentVulnIndex]) {
                vulnCards[currentVulnIndex].classList.add('active');
            }
            if (vulnDots[currentVulnIndex]) {
                vulnDots[currentVulnIndex].classList.add('active');
            }
        }

        function logout() {
            token = "";
            localStorage.removeItem("token");
            document.getElementById("loginTokenDisplay").style.display = "none";
            document.getElementById("regTokenDisplay").style.display = "none";
            showQueryStatus("Logged out", "info");
            setTimeout(function() { document.getElementById("queryStatusMsg").style.display = "none"; }, 1500);
        }

        document.getElementById("queryInput").addEventListener("keydown", function(e) {
            if (e.key === "Enter" && e.ctrlKey) runQuery();
        });

        function showPage(page) {
            // Hide all pages
            document.getElementById("homePage").style.display = "none";
            document.getElementById("docsPage").style.display = "none";
            
            // Hide all doc sections
            var sections = document.querySelectorAll(".doc-section");
            sections.forEach(function(section) {
                section.classList.remove("active");
            });
            
            // Remove active class from all nav links
            var navLinks = document.querySelectorAll(".nav-link");
            navLinks.forEach(function(link) {
                link.classList.remove("active");
            });
            
            // Show selected page and activate nav link
            if (page === "home") {
                document.getElementById("homePage").style.display = "block";
                navLinks[0].classList.add("active");
            } else if (page === "docs") {
                document.getElementById("docsPage").style.display = "block";
                navLinks[1].classList.add("active");
                // Show local installation section by default when docs page opens
                showSection('local-install');
            }
        }

        function toggleSidebar() {
            var sidebar = document.getElementById("sidebar");
            var overlay = document.getElementById("sidebarOverlay");
            sidebar.classList.toggle("active");
            overlay.classList.toggle("active");
        }

        function showSection(sectionId) {
            // Hide all doc sections
            var sections = document.querySelectorAll(".doc-section");
            sections.forEach(function(section) {
                section.classList.remove("active");
            });
            
            // Show selected section
            var targetSection = document.getElementById(sectionId);
            if (targetSection) {
                targetSection.classList.add("active");
            }
            
            // Update sidebar active state
            var sidebarItems = document.querySelectorAll(".sidebar-item");
            sidebarItems.forEach(function(item) {
                item.classList.remove("active");
            });
            
            // Find and activate the corresponding sidebar item using exact match
            var activeItem = null;
            for (var i = 0; i < sidebarItems.length; i++) {
                var item = sidebarItems[i];
                var onclickAttr = item.getAttribute('onclick');
                if (onclickAttr && onclickAttr.indexOf('showSection(\'' + sectionId + '\')') !== -1) {
                    activeItem = item;
                    break;
                }
            }
            
            if (activeItem) {
                activeItem.classList.add("active");
            }
            
            // Close sidebar after selection
            var sidebar = document.getElementById("sidebar");
            var overlay = document.getElementById("sidebarOverlay");
            if (sidebar) sidebar.classList.remove("active");
            if (overlay) overlay.classList.remove("active");
        }

        function copyToClipboard(elementId) {
            var element = document.getElementById(elementId);
            if (element) {
                var text = element.textContent || element.innerText;
                navigator.clipboard.writeText(text).then(function() {
                    // Show feedback
                    var button = event.target;
                    var originalText = button.textContent;
                    button.textContent = "✓ Copied!";
                    button.style.background = "linear-gradient(135deg, rgba(74, 222, 128, 0.3) 0%, rgba(34, 197, 94, 0.3) 100%)";
                    button.style.borderColor = "rgba(74, 222, 128, 0.5)";
                    
                    setTimeout(function() {
                        button.textContent = originalText;
                        button.style.background = "";
                        button.style.borderColor = "";
                    }, 2000);
                }).catch(function(err) {
                    // Fallback for older browsers
                    var textArea = document.createElement("textarea");
                    textArea.value = text;
                    document.body.appendChild(textArea);
                    textArea.select();
                    document.execCommand('copy');
                    document.body.removeChild(textArea);
                    
                    var button = event.target;
                    button.textContent = "✓ Copied!";
                    setTimeout(function() {
                        button.textContent = originalText;
                    }, 2000);
                });
            }
        }

        // Close sidebar when clicking outside
        document.addEventListener('click', function(event) {
            var sidebar = document.getElementById("sidebar");
            var hamburger = document.querySelector(".hamburger");
            
            if (sidebar.classList.contains("active") && 
                !sidebar.contains(event.target) && 
                !hamburger.contains(event.target)) {
                sidebar.classList.remove("active");
            }
        });

        window.addEventListener('load', initVulnSlideshow);
    </script>
</body>
</html>
)HTMLEOF";
}

void signalHandler(int signal) {
    cout << "\nShutting down server..." << endl;
    if (dbConn) PQfinish(dbConn);
    exit(0);
}

// Clean up any existing processes on port 4000
void cleanupPort() {
    cout << "Checking for existing processes on port " << PORT << "..." << endl;
    
    string cmd = "lsof -ti:" + to_string(PORT) + " 2>/dev/null | xargs kill -9 2>/dev/null";
    system(cmd.c_str());
    
    string cmd2 = "fuser -k " + to_string(PORT) + "/tcp 2>/dev/null";
    system(cmd2.c_str());
    
    sleep(1);
    
    cout << "Port cleanup complete." << endl;
}

void* keepAliveThread(void* arg) {
    int port = PORT;
    const char* url = "http://127.0.0.1/graphql";
    CURL* curl = curl_easy_init();
    
    if (!curl) return NULL;
    
    while (true) {
        sleep(480);
        if (curl) {
            string fullUrl = string("http://127.0.0.1:") + to_string(port) + "/graphql";
            curl_easy_setopt(curl, CURLOPT_URL, fullUrl.c_str());
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
            curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) {
                cerr << "[KEEPALIVE] Ping successful" << endl;
            }
        }
    }
    
    curl_easy_cleanup(curl);
    return NULL;
}

int main() {
    int serverSocket, clientSocket;
    struct sockaddr_in serverAddr, clientAddr;
    socklen_t addrLen = sizeof(clientAddr);
    char buffer[BUFFER_SIZE];
    int bytesReceived;
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    if (!connectDatabase()) {
        cerr << "Failed to connect to database. Please run: sudo -u postgres psql -f scripts/init_database.sql" << endl;
        cerr << "Or use simple mode without database" << endl;
        return 1;
    }

    loadUsersCache();
    loadAuthorsCache();
    loadBooksCache();
    loadCartCache();
    loadOrdersCache();
    loadReviewsCache();
    loadWebhooksCache();

    cout << "Loaded " << usersCache.size() << " users from database" << endl;
    cout << "Loaded " << authorsCache.size() << " authors from database" << endl;
    cout << "Loaded " << booksCache.size() << " books from database" << endl;
    cout << "Loaded " << cartCache.size() << " carts from database" << endl;
    cout << "Loaded " << ordersCache.size() << " orders from database" << endl;
    cout << "Loaded " << reviewsCache.size() << " reviews from database" << endl;
    cout << "Loaded " << webhooksCache.size() << " webhooks from database" << endl;

    cout << "Port cleanup complete." << endl;
    
    pthread_t keepalive_id;
    pthread_create(&keepalive_id, NULL, keepAliveThread, NULL);
    pthread_detach(keepalive_id);

    pthread_t ratelimit_id;
    pthread_create(&ratelimit_id, NULL, rateLimitCleanup, NULL);
    pthread_detach(ratelimit_id);
    
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        cerr << "Failed to create socket" << endl;
        return 1;
    }
    
    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(PORT);
    
    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        cerr << "Bind failed" << endl;
        return 1;
    }
    
    if (listen(serverSocket, 10) < 0) {
        cerr << "Listen failed" << endl;
        return 1;
    }
    
    signal(SIGTERM, signalHandler);
    signal(SIGINT, signalHandler);
    
    cout << "========================================" << endl;
    cout << "  Vulnerable GraphQL Bookstore API     " << endl;
    cout << "  Security Learning Environment          " << endl;
    cout << "========================================" << endl;
    cout << "Starting server on port " << PORT << endl;
    cout << "GraphQL endpoint: http://localhost:" << PORT << "/graphql" << endl;
    cout << endl;
    cout << "⚠️  WARNING: This API contains intentional security vulnerabilities." << endl;
    cout << "           DO NOT USE IN PRODUCTION!" << endl;
    cout << "========================================" << endl;
    cout << endl;
    
    while (true) {
        clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &addrLen);
        if (clientSocket < 0) {
            close(clientSocket);
            continue;
        }
        
        bytesReceived = recv(clientSocket, buffer, BUFFER_SIZE - 1, 0);
        if (bytesReceived <= 0) {
            close(clientSocket);
            continue;
        }
        
        buffer[bytesReceived] = 0;
        string request(buffer);
        
        bool isGetRequest = (request.find("GET ") == 0 || request.find("GET /") != string::npos);
        bool isHealthRequest = (request.find("GET /health") == 7);
        bool isPostRequest = (request.find("POST ") == 0 && request.find("/graphql") != string::npos);
        bool isOptionsRequest = (request.find("OPTIONS") == 0);

        if (isOptionsRequest) {
            string response = "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: POST, GET, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type, Authorization\r\nContent-Length: 0\r\n\r\n";
            send(clientSocket, response.c_str(), response.length(), 0);
            close(clientSocket);
            continue;
        }

        if (isHealthRequest) {
            string response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nOK";
            send(clientSocket, response.c_str(), response.length(), 0);
            close(clientSocket);
            continue;
        }

        if (isGetRequest) {
            string html = generateLandingHTML();
            string response = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: text/html\r\nContent-Length: " + to_string(html.length()) + "\r\n\r\n";
            response += html;
            send(clientSocket, response.c_str(), response.length(), 0);
            close(clientSocket);
            continue;
        }
        
        if (isPostRequest) {
            char clientIP[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);
            string clientIPStr(clientIP);

            string retryAfter;
            if (!checkRateLimit(clientIPStr, retryAfter)) {
                cerr << "[RATELIMIT] Rate limit exceeded for IP: " << clientIPStr << endl;
                string responseBody = "{\"errors\":[{\"message\":\"Rate limit exceeded. Try again in " + retryAfter + " seconds.\"}]}";
                string response = "HTTP/1.1 429 Too Many Requests\r\nAccess-Control-Allow-Origin: *\r\nContent-Type: application/json\r\nRetry-After: " + retryAfter + "\r\nContent-Length: " + to_string(responseBody.length()) + "\r\n\r\n" + responseBody;
                send(clientSocket, response.c_str(), response.length(), 0);
                close(clientSocket);
                continue;
            }

            cout << "[REQUEST] POST /graphql received, buffer size: " << bytesReceived << endl;

            string authHeaderStr = "";
            size_t authPos = request.find("Authorization:");
            if (authPos == string::npos) authPos = request.find("authorization:");
            if (authPos != string::npos) {
                size_t lineEnd = request.find("\r\n", authPos);
                if (lineEnd == string::npos) lineEnd = request.find("\n", authPos);
                if (lineEnd != string::npos) {
                    authHeaderStr = request.substr(authPos + 14, lineEnd - authPos - 14);
                }
            }

            if (!authHeaderStr.empty() && authHeaderStr[0] == ' ') {
                authHeaderStr = authHeaderStr.substr(1);
            }

            User currentUser = extractAuthUser(authHeaderStr);

            size_t headerEnd = request.find("\r\n\r\n");
            if (headerEnd == string::npos) headerEnd = request.find("\n\n");
            cout << "[REQUEST] headerEnd: " << (headerEnd == string::npos ? "not found" : to_string(headerEnd)) << endl;

            string queryStr = "";

            if (headerEnd != string::npos) {
                string body = request.substr(headerEnd + 4);
                cout << "[REQUEST] Body raw: " << body.substr(0, min((size_t)500, body.length())) << endl;

                size_t queryPos = body.find("\"query\"");
                if (queryPos == string::npos) queryPos = body.find("query");
                cerr << "[REQUEST] queryPos: " << (queryPos == string::npos ? "not found" : to_string(queryPos)) << endl;

                if (queryPos != string::npos) {
                    size_t colonPos = body.find(":", queryPos);
                    if (colonPos != string::npos) {
                        size_t valueStart = body.find("\"", colonPos + 1);
                        if (valueStart != string::npos && valueStart < body.length()) {
                            string value;
                            bool escaped = false;
                            for (size_t i = valueStart + 1; i < body.length(); i++) {
                                char c = body[i];
                                if (escaped) {
                                    if (c == 'n') {
                                        value += '\n';
                                    } else if (c == 't') {
                                        value += '\t';
                                    } else if (c == 'r') {
                                        value += '\r';
                                    } else if (c == '\\') {
                                        value += '\\';
                                    } else if (c != '"') {
                                        value += c;
                                    }
                                    escaped = false;
                                } else if (c == '\\') {
                                    escaped = true;
                                } else if (c == '"') {
                                    break;
                                } else {
                                    value += c;
                                }
                            }
                            queryStr = value;
                            cout << "[REQUEST] Extracted query: " << queryStr.substr(0, min((size_t)200, queryStr.length())) << endl;
                        }
                    }
                }
            }

            bool isMutation = (queryStr.find("mutation {") != string::npos || queryStr.find("mutation(") != string::npos);
            cout << "[REQUEST] isMutation: " << (isMutation ? "true" : "false") << endl;

            string responseBody = handleRequest(queryStr, currentUser, isMutation);

            cout << "[REQUEST] Response: " << responseBody << endl;

            string response = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: Content-Type, Authorization\r\nAccess-Control-Allow-Methods: POST, GET, OPTIONS\r\nContent-Type: application/json\r\nContent-Length: " +
                to_string(responseBody.length()) + "\r\nX-Content-Type-Options: nosniff\r\n\r\n" + responseBody;
            send(clientSocket, response.c_str(), response.length(), 0);
        }

        close(clientSocket);
    }
    
    if (dbConn) PQfinish(dbConn);
    curl_global_cleanup();
    
    return 0;
}
