#ifndef BOOK_MANAGER_H
#define BOOK_MANAGER_H

#include <string>
#include <map>
#include <vector>
#include <algorithm>

struct Book {
    int id;
    std::string isbn;
    std::string title;
    std::string description;
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
    std::string firstName;
    std::string lastName;
    std::string bio;
};

extern std::map<int, Book> booksCache;
extern std::map<int, Author> authorsCache;

void loadBooksCache();
void loadAuthorsCache();
Book* getBookById(int id);
Author* getAuthorById(int authorId);
std::vector<Book> searchBooks(const std::string& query, int categoryId);

#endif // BOOK_MANAGER_H
