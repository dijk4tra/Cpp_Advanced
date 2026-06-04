#include <jwt.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

const char* SECRET_KEY = "UOFJDSLJ";

void generate_jwt_token(const char* secret_key)
{
    jwt_t* jwt;
    jwt_new(&jwt);  // 创建 JWT

    // 设置算法为 HS256
    jwt_set_alg(jwt, JWT_ALG_HS256, (unsigned char*)secret_key, strlen(secret_key));

    // 设置载荷(Payload): 用户自定义数据
    jwt_add_grant(jwt, "sub", "subject");
    jwt_add_grant(jwt, "username", "peanutixx");	// 用户ID
    jwt_add_grant(jwt, "role", "admin");    		// 用户角色
    jwt_add_grant_int(jwt, "exp", time(NULL) + 3600);   // 过期时间 (1小时)

    char* token = jwt_encode_str(jwt);		// token长度是不确定的，100-300字节
    printf("Generated JWT: %s\n", token);

    // 释放资源
    jwt_free(jwt);
    free(token);
}

void verify_jwt(const char* token, const char* secret_key)
{
    jwt_t* jwt;
    int err = jwt_decode(&jwt, token, (unsigned char*)secret_key, strlen(secret_key));
    if (err) {
        printf("Invalid JWT!\n");
        return ;
    }

    // 获取 Payload
    printf("Subject: %s\n", jwt_get_grant(jwt, "sub"));
    printf("Username: %s\n", jwt_get_grant(jwt, "username"));
    printf("Role: %s\n", jwt_get_grant(jwt, "role"));
    printf("Expiration Time: %ld\n", jwt_get_grant_int(jwt, "exp"));

    jwt_free(jwt);
}

int main()
{
    // generate_jwt_token(SECRET_KEY);
    const char* token = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJleHAiOjE3ODA1ODE1MzcsInJvbGUiOiJhZG1pbiIsInN1YiI6InN1YmplY3QiLCJ1c2VybmFtZSI6InBlYW51dGl4eCJ9.bVY9kBGjKB_TBDV8Rsxc_jGvwZhOOIQBp8qWIExT7Yw";

    verify_jwt(token, SECRET_KEY);

    return 0;
}
