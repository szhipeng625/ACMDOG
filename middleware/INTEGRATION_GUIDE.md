# 客户端 ojcore 改造接入指南（HTTP 中间层模式）

## 已交付的文件（都在 mq_middleware/ 目录）

| 文件 | 作用 |
|---|---|
| `http_client.h` | Winsock 极简 HTTP 客户端（GET/POST，带 Bearer token）|
| `http_json.h` | 极简 JSON 解析（取字段/判 ok/取 error）|
| `mysql_dao_http.cpp` | 每个 mysql_* 函数的 HTTP 分支参考实现（模板）|
| 本文件 | 接入步骤 |

## 接入步骤（在 oj 的 Windows 开发环境操作）

### 1. 复制文件

把 `http_client.h`、`http_json.h` 复制到 `~/OJ/client/ojcore/` 目录。

### 2. 改造 mysql_dao.cpp

在 `mysql_dao.cpp` 顶部 `#include "http_client.h"` 和 `#include "http_json.h"`，
并加全局状态：

```cpp
// 合入 mysql_dao.cpp 顶部（namespace 内）
#include "http_client.h"
#include "http_json.h"

std::string g_mw_url;     // 中间层地址，空 = 直连模式
std::string g_mw_token;   // 登录后的 token
```

### 3. 改 mysql_connect 加分支

```cpp
bool mysql_connect(const std::string& host, unsigned int port,
                   const std::string& user, const std::string& pass,
                   const std::string& db, std::string& err) {
    // 若配置了 middlewareUrl，走 HTTP 模式，不连 libmysql
    if (!g_mw_url.empty()) {
        // 中间层模式：只需保存 token，无需真实 SQL 连接
        return true;  // 后续每个函数走 HTTP 分支
    }
    // ... 原有 libmysql 直连逻辑 ...
}
```

### 4. 每个 mysql_* 函数加 HTTP 分支

以 `mysql_register` 为例：

```cpp
bool mysql_register(const std::string& username, const std::string& password,
                    const std::string& role, std::string& err) {
    if (use_middleware()) return mw_register(username, password, role, err);
    // ... 原有直连逻辑 ...
}
```

其余函数同理，参考 `mysql_dao_http.cpp` 里的 `mw_*` 对应实现。

### 5. 改 mysql_config.json

```json
{
  "middlewareUrl": "http://47.253.41.10:8899",
  "host": "43.157.39.171",
  "port": 3306,
  "user": "oj_user",
  "pass": "12345678",
  "db": "oj",
  "redisHost": "43.157.39.171",
  "redisPort": 6380
}
```

- `middlewareUrl` 非空 → 走 HTTP 中间层（`host/user/pass` 不再用于 SQL 连接）
- `middlewareUrl` 空 → 走原直连（向后兼容，可灰度）

### 6. 读取 middlewareUrl

在 `api.cpp` 的 `oj_init_mysql` 里，从 config JSON 读 `middlewareUrl` 字段，赋给 `g_mw_url`。

## 关键点

1. **token 流转**：登录后 `g_mw_token` 保存，后续写接口请求头带 `Authorization: Bearer <token>`
2. **渐进迁移**：可先只改 `mysql_register/login/whoami/submit/board` 这几个核心函数，其余保持直连
3. **LSM**：`resp_client.cpp` 直连 LSM 的部分，若也要收口到中间层，需在中间层加对应 RESP 转发接口（本阶段暂不改，先收口 MySQL）
4. **HTTP JSON 转义**：`http_client.h`/`http_json.h` 的 body 拼接里，若 username/password/detail 含特殊字符（引号、反斜杠），需要额外 JSON 转义。生产建议用正规 JSON 库（如 nlohmann/json 或 RapidJSON）替代手拼

## 风险清单

- [ ] 手拼 JSON 需注意转义（建议换正规 JSON 库）
- [ ] HTTPS：生产环境 token 明文走 HTTP 有风险，中间层需加 TLS 或走内网
- [ ] resp_client 直连 LSM 尚未收口（第二阶段做）
- [ ] Windows 编译需在 oj 开发机完成（本机 Linux 无法编 Windows DLL）
