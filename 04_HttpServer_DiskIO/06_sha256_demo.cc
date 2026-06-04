#include "common.h"
#include <openssl/evp.h>

void sha256_hash(const char *data) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();		// 创建 EVP 上下文
    unsigned char hash[EVP_MAX_MD_SIZE];	// EVP_MAX_MD_SIZE: 最大哈希长度
    unsigned int hash_len;					// 用来接收实际的哈希长度

    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);		// 初始化上下文，采用 sha256 哈希算法
    EVP_DigestUpdate(ctx, data, strlen(data));		// 更新上下文
    EVP_DigestFinal_ex(ctx, hash, &hash_len);		// 计算哈希值

    printf("SHA-256: ");
    for (int i = 0; i < hash_len; i++) {			// 转换成十六进制字符
        printf("%02x", hash[i]); // 0000 1010 ---> 0a
    }
    printf("\n");

    EVP_MD_CTX_free(ctx);							// 释放上下文
}

int main() {
    const char *data = "Peanut loves JingTian";
    sha256_hash(data);

    return 0;
}
