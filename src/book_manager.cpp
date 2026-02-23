#include "book_manager.h"
#include "utils.h"
#include <iostream>
#include <postgresql/libpq-fe.h>
#include <algorithm>

extern PGconn* dbConn;

std::map<int, Book> booksCache;
std::map<int, Author> authorsCache;

void loadBooksCache() {
    if (dbConn == nullptr) {
        std::cout << "[DB] Cannot load books - no database connection" << std::endl;
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
            book.isFeatured = (std::string(PQgetvalue(res, i, 11)) == "t");
            book.isBestseller = (std::string(PQgetvalue(res, i, 12)) == "t");
            book.isActive = (std::string(PQgetvalue(res, i, 13)) == "t");
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

Book* getBookById(int id) {
    auto it = booksCache.find(id);
    if (it != booksCache.end()) {
        return &it->second;
    }
    return nullptr;
}

void loadAuthorsCache() {
    if (dbConn == nullptr) {
        return;
    }
    PGresult* res = PQexec(dbConn, "SELECT id, name, bio FROM authors");
    
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        for (int i = 0; i < rows; i++) {
            Author author;
            author.id = atoi(PQgetvalue(res, i, 0));
            std::string fullName = PQgetvalue(res, i, 1) ? PQgetvalue(res, i, 1) : "";
            author.bio = PQgetvalue(res, i, 2) ? PQgetvalue(res, i, 2) : "";
            
            size_t spacePos = fullName.find(' ');
            if (spacePos != std::string::npos) {
                author.firstName = fullName.substr(0, spacePos);
                author.lastName = fullName.substr(spacePos + 1);
            } else {
                author.firstName = fullName;
                author.lastName = "";
            }
            
            authorsCache[author.id] = author;
        }
    }
    PQclear(res);
}

std::vector<Book> searchBooks(const std::string& query, int categoryId) {
    std::vector<Book> results;
    for (auto& pair : booksCache) {
        if (categoryId > 0 && pair.second.categoryId != categoryId) continue;
        if (!query.empty()) {
            std::string lowerTitle = pair.second.title;
            std::transform(lowerTitle.begin(), lowerTitle.end(), lowerTitle.begin(), ::tolower);
            std::string lowerQuery = query;
            std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), ::tolower);
            if (lowerTitle.find(lowerQuery) == std::string::npos) continue;
        }
        results.push_back(pair.second);
    }
    return results;
}
