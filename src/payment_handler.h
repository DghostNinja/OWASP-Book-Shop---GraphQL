#ifndef PAYMENT_HANDLER_H
#define PAYMENT_HANDLER_H

#include <string>

bool chargeVulnBankCard(const std::string& cardNumber, const std::string& expiry, const std::string& cvv, double amount, std::string& transactionId, std::string& message);
bool recordPayment(const std::string& orderId, const std::string& userId, double amount, const std::string& transactionId, const std::string& status, std::string& errorMessage);
std::string processPayment(const std::string& userId, const std::string& orderId, double totalAmount, const std::string& cardNumber, const std::string& expiry, const std::string& cvv);

#endif // PAYMENT_HANDLER_H
