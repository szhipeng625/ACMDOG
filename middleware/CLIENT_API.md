# 客户端对接文档 —— OJ 中间层 HTTP 接口

> 版本：v1.1（2026-09-23）
> 中间层地址：`https://47.253.41.10:8899`（HTTPS，自签证书需信任或忽略校验）

---

## 0. 概述

客户端不再直连 MySQL/LSM，统一通过 HTTP 接口访问本中间层。
中间层负责：鉴权、限流、防注入、数据加密存储、落库、榜单聚合、LSM 透传。

**通用约定：**
- 请求/响应均为 JSON（`Content-Type: application/json`）
- 响应统一含 `"ok": true/false` 字段；失败时含 `"error": "错误信息"`
- 鉴权接口需在请求头带 `Authorization: Bearer <token>`
- 所有时间格式：`YYYY-MM-DD HH:MM:SS`

---

## 1. 认证

### 1.1 注册
```
POST /api/register
```
请求：
```json
{"username": "zhangsan", "password": "123456", "role": "user"}
```
- `role` 可选：`admin` / `author` / `user`（默认 user）

响应：
```json
{"ok": true, "registered": true}
```

### 1.2 登录
```
POST /api/login
```
请求：
```json
{"username": "zhangsan", "password": "123456"}
```
响应：
```json
{"ok": true, "token": "b1eb4c...a074", "userId": 4, "role": "user", "username": "zhangsan", "nickname": "zhangsan", "avatar": ""}
```
- **`token` 是后续所有鉴权接口的凭证，客户端需保存**
- `avatar` 为头像 data URL（无头像时为空串）

### 1.3 会话校验
```
GET /api/whoami?token=<token>
```
或请求头 `Authorization: Bearer <token>`

响应：
```json
{"ok": true, "userId": 4, "username": "zhangsan", "role": "user", "nickname": "zhangsan", "avatar": ""}
```

### 1.4 登出
```
POST /api/logout    (需 token)
```
响应：`{"ok": true, "logout": true}`

---

## 2. 提交与榜单

### 2.1 提交判题结果
```
POST /api/submit    (需 token)
```
请求：
```json
{
  "problemId": 111,
  "contestId": 1,
  "verdict": "AC",
  "detail": "通过 11/11 组测试点",
  "timeMs": 42,
  "virtual": false,
  "ts": "2026-09-23 10:00:00"
}
```
- `contestId` 为 0 表示练习提交
- `verdict`：`AC` / `WA` / `TLE` / `RE` / `CE` / `SE`
- `ts` 可选，客户端本地判题时间戳；不传则用服务端时间
- 服务端按 ICPC 语义处理：已 AC 的题不再被后续提交改写；AC 前每次错误提交 `wrong_count+1`

响应：
```json
{"ok": true, "sid": 22}
```
- `sid` 是本次提交的 id（与 LSM `submission:{sid}` 键对应）

### 2.2 ICPC 榜单
```
GET /api/board?cid=<比赛id>&start=<开始时间>&problems=<题目id列表>
```
参数说明：
- `cid`：比赛 id（0 = 练习）
- `start`：比赛开始时间，格式 `YYYY-MM-DD HH:MM:SS`（需 URL 编码）
- `problems`：题目 id 列表，逗号分隔，如 `111,112`

响应：
```json
{
  "official": [
    {"rank": 1, "username": "alice", "solved": 2, "penalty": 24}
  ],
  "virtual": [
    {"rank": 1, "username": "bob", "solved": 1, "penalty": 5}
  ]
}
```
- `official` 正式参赛榜，`virtual` 虚拟参赛榜
- 排名规则：AC 数降序 → 罚时升序（罚时 = 用时分钟 + 20×WA次数）

### 2.3 实时榜单（内存，推荐用于前端轮询）
```
GET /api/board/realtime?cid=<比赛id>&top=<前N名>
```
参数：
- `cid`：比赛 id（必填）
- `top`：返回前 N 名，默认 100

响应：
```json
{
  "official": [
    {"rank": 1, "username": "alice", "solved": 2, "penalty": 24}
  ],
  "virtual": [
    {"rank": 1, "username": "bob", "solved": 1, "penalty": 5}
  ]
}
```

**与 2.2 的区别**：
- 2.2 `/api/board` 是**实时查 MySQL** 算榜（准实时，每次请求都查库）
- 2.3 `/api/board/realtime` 是**纯内存榜**（微秒级，前端每 5~10 秒轮询用，无数据库压力）

**建议**：前端实时刷新榜单用 `/api/board/realtime`，性能最佳。

---

## 3. 题目

### 3.1 题目列表
```
GET /api/problems
```
响应：
```json
[
  {"id": 111, "title": "YesNum", "isPublic": true, "dataCount": 11}
]
```

### 3.2 题目详情
```
GET /api/problem?id=111
```
响应（含解密后的标程与生成器源码）：
```json
{
  "ok": true,
  "id": 111,
  "title": "YesNum",
  "description": "...",
  "sampleIn": "...",
  "sampleOut": "...",
  "timeMs": 1000,
  "memMb": 256,
  "tags": ["基础"],
  "stdCode": "#include<bits/stdc++.h>...",
  "isPublic": true,
  "generators": [
    {"id": 7, "name": "test", "description": "...", "code": "...", "genCount": 10}
  ]
}
```

### 3.3 题目可见性
```
GET /api/problem_visibility
```
响应：`{"111": true, "112": false}`

### 3.4 发布/更新题目（需 token）
```
POST /api/problem/upsert
```
请求：
```json
{
  "id": 500,
  "title": "测试题",
  "description": "...",
  "sampleIn": "...",
  "sampleOut": "...",
  "timeMs": 1000,
  "memMb": 256,
  "tags": "[\"基础\"]",
  "stdCode": "#include...",
  "isPublic": true
}
```
响应：`{"ok": true, "saved": true}`

---

## 4. 生成器

### 4.1 生成器列表
```
GET /api/generators
```
响应：`[{"id": 7, "name": "test", "description": "...", "createdAt": "..."}]`

### 4.2 生成器详情
```
GET /api/generator?id=7
```
响应：`{"ok": true, "id": 7, "name": "test", "code": "...", "description": "..."}`

### 4.3 新建生成器（需 token）
```
POST /api/generator/create
```
请求：`{"name": "juhua", "code": "...", "description": "..."}`
响应：`{"ok": true, "id": 8}`

### 4.4 更新生成器（需 token）
```
POST /api/generator/update
```
请求：`{"id": 8, "code": "...", "description": "..."}`

### 4.5 搜索生成器
```
GET /api/generator/search?q=<关键词>
```

### 4.6 绑定/解绑生成器到题目（需 token）
```
POST /api/generator/bind
POST /api/generator/unbind
```
请求：`{"problemId": 111, "generatorId": 7, "genCount": 10}`

---

## 5. 测试样例

### 5.1 写入/更新测试样例（需 token）
```
POST /api/testcase/set
```
请求：`{"problemId":111,"name":"1","input":"...","output":"...","isSample":true}`
响应：`{"ok": true, "saved": true}`

### 5.2 删除测试样例（需 token）
```
POST /api/testcase/remove
```
请求：`{"problemId":111,"name":"1"}`
响应：`{"ok": true, "removed": true}`

### 5.3 测试样例列表（公开）
```
GET /api/testcases?problemId=111
```
响应：
```json
[{"name": "1", "input": "...", "output": "...", "isSample": true}]
```

---

## 6. 比赛

### 6.1 发布比赛（需 token）
```
POST /api/contest/upsert
```
请求：`{"id": 1, "content": "{\"name\":\"...\",\"problems\":[...]}"}`

### 6.2 报名（需 token）
```
POST /api/contest/register
```
请求：`{"contestId": 1, "virtual": false}`

### 6.3 比赛提交列表
```
GET /api/contest/submissions?cid=1&username=zhangsan&view_all=0
```
- `view_all=1` 返回全部（比赛结束后用），`0` 只返回本人

### 6.4 用户进度
```
GET /api/progress?username=zhangsan&cid=1
```

### 6.5 比赛列表（公开，客户端同步用）
```
GET /api/contests
```
响应：
```json
[{"id": 1, "content": "{\"id\":1,\"name\":\"test\",\"startTime\":\"...\",\"endTime\":\"...\",\"problems\":[111]}"}]
```
- `content` 为比赛 JSON 原文，客户端物化到本地 `contests/{id}/contest.json`

### 6.6 比赛详情（公开）
```
GET /api/contest?id=1
```
响应：
```json
{"ok": true, "id": 1, "content": "{\"id\":1,\"name\":\"test\",...}"}
```

---

## 7. 用户

### 7.1 更新资料（需 token）
```
POST /api/profile
```
请求：`{"nickname": "新昵称", "avatar": "data:image/..."}`

### 7.2 用户列表（需 admin token）
```
GET /api/users
```

### 7.3 按用户名查 id
```
GET /api/user_id?username=zhangsan
```
响应：`{"ok": true, "userId": 4}`

---

## 8. LSM 透传（需 token）

客户端不再直连 LSM（6380），所有 LSM 读写经以下接口透传：

| 接口 | 请求 | 响应 |
|---|---|---|
| `POST /api/lsm/set` | `{"key":"k","value":"v"}` | `{"ok":true,"set":true}` |
| `POST /api/lsm/get` | `{"key":"k"}` | `{"ok":true,"found":true,"value":"v"}` |
| `POST /api/lsm/del` | `{"key":"k"}` | `{"ok":true,"count":1}` |
| `POST /api/lsm/incr` | `{"key":"k"}` | `{"ok":true,"value":1}` |
| `POST /api/lsm/hset` | `{"key":"k","field":"f","value":"v"}` | `{"ok":true,"set":true}` |
| `POST /api/lsm/hget` | `{"key":"k","field":"f"}` | `{"ok":true,"found":true,"value":"v"}` |
| `POST /api/lsm/hkeys` | `{"key":"k"}` | `{"ok":true,"keys":["f"]}` |

- `get`/`hget` 不存在时返回 `"found": false`（不视为错误）

---

## 9. 健康检查

```
GET /ping
```
响应：`{"ok": true, "pong": true}`

---

## 10. 限流

- 中间层按客户端 IP 限流：默认每分钟 60 请求
- 超限返回 HTTP 429：`{"ok": false, "error": "rate limit exceeded"}`
- 限流窗口结束后自动恢复

---

## 11. 客户端改造清单（ojcore）

| 原函数 | 改为调用 |
|---|---|
| mysql_register | POST /api/register |
| mysql_login | POST /api/login |
| mysql_whoami | GET /api/whoami |
| mysql_logout | POST /api/logout |
| mysql_upsert_submission | POST /api/submit |
| mysql_board | GET /api/board |
| mysql_list_problems | GET /api/problems |
| mysql_get_problem | GET /api/problem |
| mysql_problem_visibility | GET /api/problem_visibility |
| mysql_upsert_problem | POST /api/problem/upsert |
| mysql_list_generators | GET /api/generators |
| mysql_get_generator | GET /api/generator |
| mysql_create_generator | POST /api/generator/create |
| mysql_update_generator | POST /api/generator/update |
| mysql_search_generators | GET /api/generator/search |
| mysql_bind_generator | POST /api/generator/bind |
| mysql_unbind_generator | POST /api/generator/unbind |
| mysql_set_testcase | POST /api/testcase/set |
| mysql_remove_testcase | POST /api/testcase/remove |
| mysql_list_testcases | GET /api/testcases |
| mysql_upsert_contest | POST /api/contest/upsert |
| mysql_contest_register | POST /api/contest/register |
| mysql_contest_submissions | GET /api/contest/submissions |
| mysql_user_progress | GET /api/progress |
| mysql_list_users | GET /api/users |
| mysql_update_profile | POST /api/profile |
| mysql_user_id_by_name | GET /api/user_id |
| mysql_sync_problems | GET /api/problems + /api/problem + /api/testcases + /api/contests |
| RespClient.*（LSM） | POST /api/lsm/{set,get,del,incr,hset,hget,hkeys} |

**关键配置**：`mysql_config.json` 加 `"middlewareUrl": "http://47.253.41.10:8899"`（必填，客户端只走中间层，不再直连 MySQL/LSM）。

---

## 12. 注意事项

1. **token**：登录后保存 token，写接口必须带 `Authorization: Bearer <token>`，7 天过期。
2. **JSON 转义**：代码/题面/详情等字段含引号、换行时，务必正确 JSON 转义（建议用正规 JSON 库，如 nlohmann/json）。
3. **数据加密**：标程 `stdCode` 和生成器 `code` 由中间层自动 AES 加密存储，客户端普通收发即可（不用自己加密）。
4. **verdict 大写**：统一用 `AC`/`WA`/`TLE`/`RE`/`CE`/`SE`。
5. **HTTPS**：生产环境建议中间层加 TLS，避免 token 明文传输。
6. **收口**：MySQL 与 LSM 均已收口到中间层，客户端不再直连、不再依赖 `libmysql.dll`。
