# 客户端 ojcore 改造方案（直连 MySQL/LSM → HTTP 中间层）

## 目标

让客户端不再直接持有 MySQL/LSM 连接，统一走本机中间层（`:8899`）。
MySQL 凭据收敛到中间层，客户端只持有中间层地址 + 登录 token。

## 现状（ojcore 架构）

```
client.exe (C#) → P/Invoke → ojcore.dll (C++)
  ├ mysql_dao.cpp  → 动态加载 libmysql.dll，直接连 MySQL
  └ resp_client.cpp → Winsock 裸 socket，直接连 LSM(6380)
```

- `oj_init_mysql(host, port, user, pass, db, problem_dir)` 从 `mysql_config.json` 读连接参数
- 27 个 `mysql_*` 函数，底层依赖 `ExecSQL()` / `QueryScalar()` / `g_sql.mysql_query`
- 已有 `resp_client.cpp`（Winsock socket 实现），可复用其 socket 方式写 HTTP 客户端

## 改造方案（最小侵入）

### 1. 新增 `http_client.h`（Winsock 极简 HTTP 客户端）

复用 resp_client.cpp 的 socket 手法，实现同步 HTTP POST/GET，返回 JSON 字符串。

### 2. `mysql_config.json` 增加字段

```json
{
  "middlewareUrl": "http://47.253.41.10:8899",
  "host": "43.157.39.171",  // 遗留，中间层模式忽略
  ...
}
```

### 3. `mysql_connect` 加分支

检测到 `middlewareUrl` 非空 → 走 HTTP 模式（`g_use_middleware = true`，
记录 middleware 地址），否则走原 libmysql 直连（向后兼容）。

### 4. 业务函数改造

关键：`ExecSQL` / `QueryScalar` 这两个底层函数是 SQL 字符串执行。
HTTP 模式下，改成调用中间层对应接口。

**难点**：中间层接口是「面向业务」的（`/api/submit`、`/api/board`），
而客户端业务函数是「面向 SQL」的。所以有两种改法：

- **方案 A（推荐）**：在客户端 `mysql_*` 函数体内，当 `g_use_middleware`
  时，直接调中间层的业务接口（`HttpPost("/api/submit", json)`），
  不再拼 SQL。每个函数写一个 HTTP 分支。

- **方案 B**：中间层加一个「通用 SQL 执行接口」`/api/sql`，
  客户端把 SQL 字符串发过去执行。这样客户端改动最小（只改 ExecSQL/QueryScalar 两个函数），
  但 `/api/sql` 是**严重安全漏洞**（等于把数据库暴露给能连中间层的任何人），**不推荐**。

**结论：走方案 A**。客户端每个 `mysql_*` 函数在 HTTP 模式下调中间层业务接口。

## 中间层接口 ↔ 客户端函数映射

| 客户端函数 | 中间层接口 |
|---|---|
| mysql_register | POST /api/register |
| mysql_login | POST /api/login |
| mysql_whoami | GET /api/whoami |
| mysql_logout | POST /api/logout |
| mysql_upsert_submission | POST /api/submit |
| mysql_board | GET /api/board |
| mysql_list_problems | GET /api/problems |
| mysql_get_problem | GET /api/problem?id= |
| mysql_problem_visibility | GET /api/problem_visibility |
| mysql_upsert_problem | POST /api/problem/upsert |
| mysql_list_generators | GET /api/generators |
| mysql_get_generator | GET /api/generator?id= |
| mysql_create_generator | POST /api/generator/create |
| mysql_update_generator | POST /api/generator/update |
| mysql_search_generators | GET /api/generator/search?q= |
| mysql_bind_generator | POST /api/generator/bind |
| mysql_unbind_generator | POST /api/generator/unbind |
| mysql_set_testcase | POST /api/testcase/set |
| mysql_list_testcases | GET /api/testcases?problemId= |
| mysql_upsert_contest | POST /api/contest/upsert |
| mysql_contest_register | POST /api/contest/register |
| mysql_contest_submissions | GET /api/contest/submissions |
| mysql_user_progress | GET /api/progress |
| mysql_list_users | GET /api/users |
| mysql_update_profile | POST /api/profile |
| mysql_user_id_by_name | （需中间层加接口）|
| mysql_sync_problems | （需中间层加接口，或本地物化）|

## 实施步骤

1. 在 oj 上 `git` 拉取最新代码，备份 `mysql_dao.cpp`
2. 新增 `http_client.h`（Winsock HTTP）
3. 改造 `mysql_connect` + `mysql_available`（判断模式）
4. 逐个改造 27 个 `mysql_*` 函数（HTTP 分支 + 原直连分支保留）
5. 更新 `mysql_config.json` 加 `middlewareUrl`
6. 在 oj 的 Windows 构建环境编译测试

## 待补的中间层接口

- `mysql_user_id_by_name` → 中间层加 `GET /api/user_id?username=`
- `mysql_sync_problems` → 这个涉及本地文件物化，需单独设计

## 风险与注意

- Windows 客户端需在 oj 的开发环境编译（本机是 Linux，无法编译 Windows DLL）
- 改动涉及 oj 上的源码，需你确认是否切换到中间层模式（可灰度：先少数客户端改）
- token 需在 HTTP 请求头带 `Authorization: Bearer <token>`（或 query ?token=）
