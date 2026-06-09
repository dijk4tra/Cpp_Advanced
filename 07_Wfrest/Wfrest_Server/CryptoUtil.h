#pragma once // 避免头文件重复包含

#include <jwt.h>
#include <openssl/evp.h>
#include <string>

struct User {
    int id;
    std::string username;
    std::string hashcode;
    std::string salt;
    std::string createdAt;
    // ...
};

// 静态工具类
class CryptoUtil {
public:
    static std::string generate_salt(int length = 8);
    static std::string hash_password(const std::string& password, const std::string& salt, const EVP_MD* md = EVP_sha256());
    static std::string generate_token(const User& user, jwt_alg_t algorithm = JWT_ALG_HS256);
    static bool verify_token(const std::string& token, User& user);

private:
    CryptoUtil() = delete;
};
