#include <jwt.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <stdlib.h>
#include <string.h>

#include "CryptoUtil.h"

using namespace std;

static const char* SECRET_KEY = "$Rv&O98@";

string CryptoUtil::generate_salt(int length)
{
    const char* alpha = "0123456789"
                        "abcdefghijklmnopqrstuvwxyz"
                        "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    string result;
    for (int i = 0; i < length; ++i) {
        result += alpha[rand() % 62];
    }
    return result;
}

string CryptoUtil::hash_password(const string& password, const string& salt, const EVP_MD* md)
{
    EVP_MD_CTX* context = EVP_MD_CTX_new();

    EVP_DigestInit_ex(context, md, NULL);
    EVP_DigestUpdate(context, salt.c_str(), salt.size());
    EVP_DigestUpdate(context, password.c_str(), password.size());

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_DigestFinal(context, hash, &len);

    char result[EVP_MAX_MD_SIZE * 2 + 1] = { '\0' };
    for (unsigned i = 0; i < len; i++) {
        sprintf(result + 2 * i, "%02x", hash[i]);
    }

    EVP_MD_CTX_free(context);

    return result;
}

string CryptoUtil::generate_token(const User& user, jwt_alg_t algorithm)
{
    jwt_t* jwt;
    jwt_new(&jwt);

    jwt_set_alg(jwt, algorithm, (unsigned char*)SECRET_KEY, strlen(SECRET_KEY));

    // JWT payload 只放身份标识和过期时间，不放敏感数据。
    jwt_add_grant(jwt, "sub", "LoginToken");
    jwt_add_grant_int(jwt, "id", user.id);
    jwt_add_grant(jwt, "username", user.username.c_str());
    jwt_add_grant(jwt, "created_at", user.createdAt.c_str());
    jwt_add_grant_int(jwt, "exp", time(NULL) + 3600);

    char* token = jwt_encode_str(jwt);
    string result = token;

    jwt_free(jwt);
    free(token);

    return result;
}

bool CryptoUtil::verify_token(const string& token, User& user)
{
    jwt_t* jwt;
    int err = jwt_decode(&jwt, token.c_str(), (unsigned char*)SECRET_KEY, strlen(SECRET_KEY));
    if (err) {
        return false;
    }

    const char* subject = jwt_get_grant(jwt, "sub");
    if (subject == nullptr || strcmp(subject, "LoginToken") != 0) {
        jwt_free(jwt);
        return false;
    }

    long expire = jwt_get_grant_int(jwt, "exp");
    if (expire < time(NULL)) {
        jwt_free(jwt);
        return false;
    }

    user.id = jwt_get_grant_int(jwt, "id");
    user.username = jwt_get_grant(jwt, "username");
    user.createdAt = jwt_get_grant(jwt, "created_at");

    jwt_free(jwt);
    return true;
}

std::string CryptoUtil::generate_hashcode(const char* data, size_t n, const EVP_MD* md)
{
    EVP_MD_CTX* context = EVP_MD_CTX_new();

    EVP_DigestInit_ex(context, md, NULL);

    EVP_DigestUpdate(context, data, n);

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_DigestFinal(context, hash, &len);

    char result[EVP_MAX_MD_SIZE * 2 + 1] = { '\0' };
    for (unsigned i = 0; i < len; i++) {
        sprintf(result + 2 * i, "%02x", hash[i]);
    }
    EVP_MD_CTX_free(context);

    return result;
}
