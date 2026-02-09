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
map<int, Review> reviewsCache;
map<string, vector<CartItem>> cartCache;
map<string, Order> ordersCache;
map<string, Webhook> webhooksCache;

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

bool connectDatabase() {
    string connStr = DB_CONN;
    if (connStr.find("://") != string::npos) {
        cerr << "[DB] Using URL connection string: " << connStr << endl;
    } else {
        cerr << "[DB] Using keyword connection string: " << connStr << endl;
    }

    dbConn = PQconnectdb(connStr.c_str());
    if (PQstatus(dbConn) != CONNECTION_OK) {
        cerr << "[DB] Database connection FAILED: " << PQerrorMessage(dbConn) << endl;
        return false;
    }
    return true;
}

bool checkDatabaseConnection() {
    if (dbConn == nullptr) {
        return connectDatabase();
    }
    ConnStatusType status = PQstatus(dbConn);
    if (status != CONNECTION_OK) {
        PQfinish(dbConn);
        dbConn = nullptr;
        return connectDatabase();
    }
    return true;
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

string userToJson(const User& user) {
    stringstream ss;
    ss << "{";
    ss << "\"id\":\"" << user.id << "\",";
    ss << "\"username\":\"" << user.username << "\",";
    ss << "\"firstName\":\"" << user.firstName << "\",";
    ss << "\"lastName\":\"" << user.lastName << "\",";
    ss << "\"role\":\"" << user.role << "\",";
    ss << "\"isActive\":" << (user.isActive ? "true" : "false") << ",";
    ss << "\"phone\":\"" << user.phone << "\",";
    ss << "\"address\":\"" << escapeJson(user.address) << "\",";
    ss << "\"city\":\"" << user.city << "\",";
    ss << "\"state\":\"" << user.state << "\",";
    ss << "\"zipCode\":\"" << user.zipCode << "\",";
    ss << "\"country\":\"" << user.country << "\"";
    ss << "}";
    return ss.str();
}

string bookToJson(const Book& book) {
    stringstream ss;
    ss << "{";
    ss << "\"id\":" << book.id << ",";
    ss << "\"isbn\":\"" << book.isbn << "\",";
    ss << "\"title\":\"" << escapeJson(book.title) << "\",";
    ss << "\"description\":\"" << escapeJson(book.description) << "\",";
    ss << "\"authorId\":" << book.authorId << ",";
    ss << "\"categoryId\":" << book.categoryId << ",";
    ss << "\"price\":" << book.price << ",";
    ss << "\"salePrice\":" << book.salePrice << ",";
    ss << "\"stockQuantity\":" << book.stockQuantity << ",";
    ss << "\"ratingAverage\":" << book.ratingAverage << ",";
    ss << "\"reviewCount\":" << book.reviewCount << ",";
    ss << "\"isFeatured\":" << (book.isFeatured ? "true" : "false") << ",";
    ss << "\"isBestseller\":" << (book.isBestseller ? "true" : "false") << ",";
    ss << "\"isActive\":" << (book.isActive ? "true" : "false");
    ss << "}";
    return ss.str();
}

string cartItemToJson(const CartItem& item) {
    stringstream ss;
    ss << "{";
    ss << "\"id\":" << item.id << ",";
    ss << "\"cartId\":\"" << item.cartId << "\",";
    ss << "\"bookId\":" << item.bookId << ",";
    ss << "\"quantity\":" << item.quantity << ",";
    ss << "\"price\":" << item.price;
    ss << "}";
    return ss.str();
}

string orderItemToJson(const OrderItem& item) {
    stringstream ss;
    ss << "{";
    ss << "\"id\":" << item.id << ",";
    ss << "\"orderId\":\"" << item.orderId << "\",";
    ss << "\"bookId\":" << item.bookId << ",";
    ss << "\"bookTitle\":\"" << escapeJson(item.bookTitle) << "\",";
    ss << "\"bookIsbn\":\"" << item.bookIsbn << "\",";
    ss << "\"quantity\":" << item.quantity << ",";
    ss << "\"unitPrice\":" << item.unitPrice << ",";
    ss << "\"totalPrice\":" << item.totalPrice;
    ss << "}";
    return ss.str();
}

string orderToJson(const Order& order) {
    stringstream ss;
    ss << "{";
    ss << "\"id\":\"" << order.id << "\",";
    ss << "\"userId\":\"" << order.userId << "\",";
    ss << "\"orderNumber\":\"" << order.orderNumber << "\",";
    ss << "\"status\":\"" << order.status << "\",";
    ss << "\"subtotal\":" << order.subtotal << ",";
    ss << "\"taxAmount\":" << order.taxAmount << ",";
    ss << "\"shippingAmount\":" << order.shippingAmount << ",";
    ss << "\"discountAmount\":" << order.discountAmount << ",";
    ss << "\"totalAmount\":" << order.totalAmount << ",";
    ss << "\"shippingAddress\":\"" << escapeJson(order.shippingAddress) << "\",";
    ss << "\"billingAddress\":\"" << escapeJson(order.billingAddress) << "\",";
    ss << "\"paymentStatus\":\"" << order.paymentStatus << "\",";
    ss << "\"createdAt\":\"" << order.createdAt << "\",";
    ss << "\"items\":[";
    for (size_t i = 0; i < order.items.size(); i++) {
        if (i > 0) ss << ",";
        ss << orderItemToJson(order.items[i]);
    }
    ss << "]";
    ss << "}";
    return ss.str();
}

string reviewToJson(const Review& review) {
    stringstream ss;
    ss << "{";
    ss << "\"id\":" << review.id << ",";
    ss << "\"userId\":\"" << review.userId << "\",";
    ss << "\"bookId\":" << review.bookId << ",";
    ss << "\"rating\":" << review.rating << ",";
    ss << "\"comment\":\"" << escapeJson(review.comment) << "\",";
    ss << "\"isVerifiedPurchase\":" << (review.isVerifiedPurchase ? "true" : "false") << ",";
    ss << "\"isApproved\":" << (review.isApproved ? "true" : "false") << ",";
    ss << "\"createdAt\":\"" << review.createdAt << "\"";
    ss << "}";
    return ss.str();
}

string webhookToJson(const Webhook& webhook) {
    stringstream ss;
    ss << "{";
    ss << "\"id\":\"" << webhook.id << "\",";
    ss << "\"userId\":\"" << webhook.userId << "\",";
    ss << "\"url\":\"" << webhook.url << "\",";
    ss << "\"secret\":\"" << webhook.secret << "\",";
    ss << "\"isActive\":" << (webhook.isActive ? "true" : "false");
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
        response << "{\"name\":\"_adminAllPayments\"}";
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
            response << "\"me\":" << userToJson(currentUser);
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
            response << "\"book\":" << bookToJson(*book);
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
            response << bookToJson(books[i]);
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
                response << userToJson(pair.second);
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
        response << "\"userId\":\"" << currentUser.id << "\",";
        response << "\"items\":[";

        bool firstItem = true;
        for (auto& pair : cartCache) {
            if (pair.first == cartId) {
                for (const auto& item : pair.second) {
                    if (!firstItem) response << ",";
                    response << cartItemToJson(item);
                    firstItem = false;
                }
            }
        }
        response << "]";
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
                response << orderToJson(pair.second);
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
                response << reviewToJson(pair.second);
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
                response << reviewToJson(pair.second);
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
                response << webhookToJson(pair.second);
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
            response << orderToJson(pair.second);
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
                response << bookToJson(book);
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
                    response << "\"user\":" << userToJson(newUser);
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
                response << "\"user\":" << userToJson(*user);
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
        response << "\"updateProfile\":" << userToJson(currentUser);
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
           "    <title>GraphQL Playground - Vulnerable Bookstore API</title>\n"
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
           "        <textarea id=\"query\" rows=\"15\" cols=\"80\" placeholder=\"Enter your GraphQL query here...\">query {\n  books {\n    id\n    title\n    price\n    stockQuantity\n  }\n}</textarea>\n"
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
           "            status.innerHTML = `<div class=\"status \${isError ? 'error' : 'success'}\">\${message}</div>`;\n"
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
           "            const query = `mutation { register(username: \"\${username}\", firstName: \"\${firstName}\", lastName: \"\${lastName}\", password: \"\${password}\") { success message token user { id username role } } }`;\n"
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
           "            const query = `mutation { login(username: \"\${username}\", password: \"\${password}\") { success message token user { id username role } } }`;\n"
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
    loadBooksCache();
    loadCartCache();
    loadOrdersCache();
    loadReviewsCache();
    loadWebhooksCache();

    cout << "Loaded " << usersCache.size() << " users from database" << endl;
    cout << "Loaded " << booksCache.size() << " books from database" << endl;
    cout << "Loaded " << cartCache.size() << " carts from database" << endl;
    cout << "Loaded " << ordersCache.size() << " orders from database" << endl;
    cout << "Loaded " << reviewsCache.size() << " reviews from database" << endl;
    cout << "Loaded " << webhooksCache.size() << " webhooks from database" << endl;
    
    cout << "Port cleanup complete." << endl;
    
    pthread_t keepalive_id;
    pthread_create(&keepalive_id, NULL, keepAliveThread, NULL);
    pthread_detach(keepalive_id);
    
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
    cout << "Access GraphQL Playground at: http://localhost:" << PORT << "/" << endl;
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
        bool isPostRequest = (request.find("POST ") == 0 && request.find("/graphql") != string::npos);
        
        if (isGetRequest) {
            string html = generatePlaygroundHTML();
            string response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " + to_string(html.length()) + "\r\n\r\n";
            response += html;
            send(clientSocket, response.c_str(), response.length(), 0);
            close(clientSocket);
            continue;
        }
        
        if (isPostRequest) {
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

            FILE* debugFile = fopen("/tmp/auth_debug.txt", "a");
            if (debugFile) {
                fprintf(debugFile, "Auth header: [%s]\n", authHeaderStr.c_str());
                fprintf(debugFile, "Current user id: [%s]\n", currentUser.id.c_str());
                fclose(debugFile);
            }
            
            size_t headerEnd = request.find("\r\n\r\n");
            if (headerEnd == string::npos) headerEnd = request.find("\n\n");

            string queryStr = "";
            if (headerEnd != string::npos) {
                size_t bodyStart = request.find("{", headerEnd);
                if (bodyStart != string::npos) {
                    size_t bodyEnd = request.rfind("}");
                    if (bodyEnd != string::npos && bodyEnd > bodyStart) {
                        string body = request.substr(bodyStart, bodyEnd - bodyStart + 1);
                        queryStr = extractQueryFromBody(body);
                        if (queryStr.empty()) {
                            queryStr = body;
                        }
                    }
                }
            }

            bool isMutation = (queryStr.find("mutation {") != string::npos || queryStr.find("mutation(") != string::npos);

            string responseBody = handleRequest(queryStr, currentUser, isMutation);

            string response = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: Content-Type, Authorization\r\nAccess-Control-Allow-Methods: POST, GET, OPTIONS\r\nContent-Type: application/json\r\nContent-Length: " +
                to_string(responseBody.length()) + "\r\nX-Content-Type-Options: nosniff\r\n\r\n" + responseBody;
            send(clientSocket, response.c_str(), response.length(), 0);
        } else if (request.find("OPTIONS") == 0) {
            string response = "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: POST, GET, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type, Authorization\r\nContent-Length: 0\r\n\r\n";
            send(clientSocket, response.c_str(), response.length(), 0);
        }
        
        close(clientSocket);
    }
    
    if (dbConn) PQfinish(dbConn);
    curl_global_cleanup();
    
    return 0;
}
