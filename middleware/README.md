# OJ 中间层 HTTP 网关（C++ 生产级）

部署在本机，唯一持有 MySQL 凭据，覆盖 OJ 全功能接口。客户端不再直连 MySQL/LSM。

## 架构

```
客户端 (client.exe / ojcore.dll)
   ↓ HTTPS/JSON（Bearer token 鉴权）
[中间层 C++ 网关] ← 本机 :8899
   ├ 鉴权 (sessions token)
   ├ 限流 (滑动窗口，按 IP)
   ├ AES-256-CBC 加密标程/生成器源码
   ├ prepared statement 防注入
   └ 全部业务逻辑（复刻 ojcore）
   ↓
oj: MySQL 3306 + LSM 6380
```

## 接口清单

### 公共（无需鉴权）
| 方法 | 路径 | 功能 |
|---|---|---|
| GET | /ping | 健康检查 |
| POST | /api/register | 注册 |
| POST | /api/login | 登录 → token |
| GET | /api/board | ICPC 榜单 (?cid=&start=&problems=) |
| GET | /api/problems | 题目列表 |
| GET | /api/problem | 题目详情 (?id=) |
| GET | /api/problem_visibility | 题目可见性 |
| GET | /api/generators | 生成器列表 |
| GET | /api/generator | 生成器详情 (?id=) |
| GET | /api/generator/search | 搜索生成器 (?q=) |
| GET | /api/testcases | 测试样例 (?problemId=) |
| GET | /api/contest/submissions | 比赛提交 (?cid=&username=&view_all=) |
| GET | /api/progress | 用户进度 (?username=&cid=) |

### 鉴权（需 Bearer token）
| 方法 | 路径 | 功能 |
|---|---|---|
| GET | /api/whoami | 会话校验 |
| POST | /api/logout | 登出 |
| POST | /api/submit | 提交判题结果 |
| POST | /api/profile | 更新资料 |
| GET | /api/users | 用户列表（admin）|
| POST | /api/problem/upsert | 发布/更新题目 |
| POST | /api/generator/create | 新建生成器 |
| POST | /api/generator/update | 更新生成器 |
| POST | /api/generator/bind | 绑定生成器到题 |
| POST | /api/generator/unbind | 解绑 |
| POST | /api/testcase/set | 设置测试样例 |
| POST | /api/contest/upsert | 发布比赛 |
| POST | /api/contest/register | 报名比赛 |

## 编译运行

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
./build/middleware config.json   # 监听 0.0.0.0:8899
```

## systemd 部署

```bash
sudo cp mq_middleware.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now mq_middleware
sudo systemctl status mq_middleware
```

## 安全机制

1. **鉴权**：write 接口需 Bearer token（sessions 表，7 天过期）
2. **限流**：滑动窗口，每 IP 每分钟 60 请求（可在 config.json 配置）
3. **防注入**：prepared statement + 转义
4. **加密**：标程/生成器源码 AES-256-CBC 密文存储（key/iv 与客户端硬编码一致）
5. **并发安全**：DB 写操作加 mutex 串行化

## 配置（config.json）

```json
{
  "http_port": 8899,
  "mysql": {"host":"...", "port":3306, "user":"...", "pass":"...", "db":"oj"},
  "lsm": {"host":"...", "port":6380},
  "rate_limit": {"max":60, "window":60}
}
```

## 依赖

- cpp-httplib（/tmp/httplib.h）
- nlohmann/json（/tmp/json.hpp）
- mysql-devel（libmysqlclient）
- OpenSSL（EVP，用于 AES）

## 待办

- [ ] 客户端 ojcore 改造：mysql_dao.cpp 直连 → HTTP 调中间层
- [ ] admin 角色校验（当前仅要求登录，未细分角色）
- [ ] 生产 token 安全管理（HTTPS 强制）
- [ ] 连接池（当前 DB 单连接 + mutex，高并发下可优化）
