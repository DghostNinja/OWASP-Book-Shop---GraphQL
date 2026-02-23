#ifndef ORDER_MANAGER_H
#define ORDER_MANAGER_H

#include <string>
#include <vector>
#include <map>

struct CartItem {
    int id;
    std::string cartId;
    int bookId;
    int quantity;
    double price;
};

struct OrderItem {
    int id;
    std::string orderId;
    int bookId;
    std::string bookTitle;
    std::string bookIsbn;
    int quantity;
    double unitPrice;
    double totalPrice;
};

struct Order {
    std::string id;
    std::string userId;
    std::string orderNumber;
    std::string status;
    double subtotal;
    double taxAmount;
    double shippingAmount;
    double discountAmount;
    double totalAmount;
    std::string shippingAddress;
    std::string billingAddress;
    std::string paymentStatus;
    std::string createdAt;
    std::vector<OrderItem> items;
};

extern std::map<std::string, std::vector<CartItem>> cartCache;
extern std::map<std::string, Order> ordersCache;

void loadCartCache();
void loadOrdersCache();

#endif // ORDER_MANAGER_H
