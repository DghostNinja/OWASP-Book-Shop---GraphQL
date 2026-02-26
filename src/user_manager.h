#ifndef USER_MANAGER_H
#define USER_MANAGER_H

#include <string>
#include <map>
#include <vector>

struct User {
    std::string id;
    std::string username;
    std::string passwordHash;
    std::string firstName;
    std::string lastName;
    std::string role;
    bool isActive;
    std::string phone;
    std::string address;
    std::string city;
    std::string state;
    std::string zipCode;
    std::string country;
};

struct AuthResult {
    User user;
    bool valid;
    std::string error;
};

extern std::map<std::string, User> usersCache;

void loadUsersCache();
User* getUserByUsername(const std::string& username);
std::string generateJWT(const User& user);
User verifyJWT(const std::string& token);
AuthResult verifyJWTWithError(const std::string& token);

#endif // USER_MANAGER_H
