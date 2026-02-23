#include "user_manager.h"
#include "utils.h"
#include <iostream>
#include <postgresql/libpq-fe.h>
#include <jwt.h>
#include <cstring>
#include <ctime>

// Global database connection from main.cpp
extern PGconn* dbConn;

// Definition of usersCache
std::map<std::string, User> usersCache;

void loadUsersCache() {
    if (dbConn == nullptr) {
        std::cerr << "[DB] Cannot load users - no database connection" << std::endl;
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
            user.isActive = (std::string(PQgetvalue(res, i, 6)) == "t");
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

User* getUserByUsername(const std::string& username) {
    auto it = usersCache.find(username);
    if (it != usersCache.end()) {
        return &it->second;
    }
    return nullptr;
}

std::string generateJWT(const User& user) {
    jwt_t* jwt = nullptr;
    int rc = jwt_new(&jwt);
    if (rc != 0) return "";
    
    jwt_add_grant(jwt, "sub", user.id.c_str());
    jwt_add_grant(jwt, "username", user.username.c_str());
    jwt_add_grant(jwt, "role", user.role.c_str());
    
    time_t now = time(nullptr);
    jwt_add_grant_int(jwt, "iat", now);
    jwt_add_grant_int(jwt, "exp", now + 21600);
    
    // JWT_SECRET is currently a macro in main.cpp, need to make it accessible
    // For now, hardcode it or pass it. Will address this when modularizing main.cpp constants.
    const char* JWT_SECRET = getenv("JWT_SECRET") ? getenv("JWT_SECRET") : "CHANGE_ME_IN_PRODUCTION_real_jwt_secret_key_2024";
    jwt_set_alg(jwt, JWT_ALG_HS256, (unsigned char*)JWT_SECRET, strlen(JWT_SECRET));
    
    char* token = jwt_encode_str(jwt);
    jwt_free(jwt);
    
    return token ? token : "";
}

User verifyJWT(const std::string& token) {
    User user;

    jwt_t* jwt = nullptr;

    // JWT_SECRET is currently a macro in main.cpp, need to make it accessible
    const char* JWT_SECRET = getenv("JWT_SECRET") ? getenv("JWT_SECRET") : "CHANGE_ME_IN_PRODUCTION_real_jwt_secret_key_2024";
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
