#include <asio.hpp>
#include <asio/ts/buffer.hpp>
#include <asio/ts/internet.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <csignal>
// 假设这些头文件和实现已经存在，并且与您的 Muduo 版本兼容
#include "redis_wrapper/redis_wrapper.h"
#include "../include/handler.h"

asio::io_context* g_io_context = nullptr;

void signalHandler(int sig) {
  if (g_io_context) {
    g_io_context->stop();
  }
}

// 简单的日志宏，替代 Muduo 的 LOG_INFO
#define ASYNC_REDIS_SERVER_LOG_INFO(msg)                                       \
  std::cout << "[INFO] " << msg << std::endl;
#define ASYNC_REDIS_SERVER_LOG_DEBUG(                                          \
    msg) /* std::cout << "[DEBUG] " << msg << std::endl; */ // 在发布版本中可以注释掉

using asio::ip::tcp;

// 安全获取客户端 endpoint 字符串；连接已断开时 remote_endpoint() 会抛异常，
// 这里捕获并返回占位字符串，避免异常逃逸导致进程崩溃。
static std::string safe_peer(const tcp::socket &socket) {
  try {
    if (!socket.is_open()) return "<closed>";
    std::error_code ec;
    auto ep = socket.remote_endpoint(ec);
    if (ec) return "<unavailable>";
    return ep.address().to_string() + ":" + std::to_string(ep.port());
  } catch (...) {
    return "<unavailable>";
  }
}

// 每个客户端连接的会话类
class RedisSession : public std::enable_shared_from_this<RedisSession> {
public:
  RedisSession(tcp::socket socket, RedisWrapper &redis_db)
      : socket_(std::move(socket)), redis_(redis_db) {}

  void start() {
    ASYNC_REDIS_SERVER_LOG_INFO("Connection from " << safe_peer(socket_));
    do_read();
  }

private:
  // 从 buffer_ 取一行（去掉行尾 \r\n）；不足一行返回 false
  bool read_line(std::string& out) {
    std::istream is(&buffer_);
    std::string line;
    if (!std::getline(is, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    out = std::move(line);
    return true;
  }

  // 递归读取剩余 total_lines 行，拼出完整请求后交给 handleRequest
  void read_rest(int total_lines, int done, std::string full_request) {
    auto self(shared_from_this());
    if (done >= total_lines) {
      ASYNC_REDIS_SERVER_LOG_DEBUG("Received message:\n" << full_request);
      std::string resp;
      try {
        resp = handleRequest(full_request);
      } catch (const std::exception &e) {
        ASYNC_REDIS_SERVER_LOG_INFO("Exception in handleRequest: " << e.what());
        resp = "-ERR internal error\r\n";
      } catch (...) {
        resp = "-ERR internal error\r\n";
      }
      do_write(resp);
      return;
    }
    asio::async_read_until(socket_, buffer_, "\r\n",
        [this, self, total_lines, done, full_request](const asio::error_code& ec, std::size_t) mutable {
            if (ec) {
              ASYNC_REDIS_SERVER_LOG_INFO("Error on read: " << ec.message());
              return;
            }
            std::string line;
            if (!read_line(line)) {
              do_write("-ERR Protocol error: partial request received\r\n");
              return;
            }
            full_request += line + "\r\n";
            read_rest(total_lines, done + 1, full_request);
        });
  }

  void do_read() {
    auto self(shared_from_this());
    asio::async_read_until(socket_, buffer_, "\r\n",
        [this, self](const asio::error_code& ec, std::size_t) {
            if (ec) {
              if (ec == asio::error::eof || ec == asio::error::connection_reset) {
                ASYNC_REDIS_SERVER_LOG_INFO("Connection closed from "
                    << safe_peer(socket_));
              } else {
                ASYNC_REDIS_SERVER_LOG_INFO("Error on read: " << ec.message());
              }
              return;
            }

            std::string first;
            if (!read_line(first)) { do_write("-ERR Protocol error\r\n"); return; }

            if (first == "PING") { do_write("+PONG\r\n"); return; }
            if (first.empty() || first[0] != '*') {
              do_write("-ERR Protocol error: expected '*'\r\n"); return;
            }
            int numElements = 0;
            try { numElements = std::stoi(first.substr(1)); }
            catch (const std::exception&) {
              do_write("-ERR Protocol error: invalid number of elements\r\n"); return;
            }

            read_rest(numElements * 2, 0, first + "\r\n");
        });
  }

  void do_write(const std::string &response) {
    auto self(shared_from_this());
    asio::async_write(
        socket_, asio::buffer(response),
        [this, self, response](const asio::error_code &ec,
                               std::size_t /*bytes_transferred*/) {
          if (!ec) {
            ASYNC_REDIS_SERVER_LOG_DEBUG("Sent response:\n" << response);
            do_read(); // 继续读取下一个请求
          } else if (ec == asio::error::eof ||
                     ec == asio::error::connection_reset) {
            ASYNC_REDIS_SERVER_LOG_INFO(
                "Connection closed from " << safe_peer(socket_));
          } else {
            ASYNC_REDIS_SERVER_LOG_INFO("Error on write: " << ec.message());
          }
        });
  }

  // 与 Muduo 版本相同的 handleRequest 逻辑
  std::string handleRequest(const std::string &request) {
    size_t pos = 0;

    if (request.empty()) {
      return "-ERR Protocol error: expected '*'\r\n";
    }

    if (request == "PING\r\n") {
      return "+PONG\r\n";
    }

    int numElements = 0;
    try {
      numElements = std::stoi(request.substr(pos + 1)); // 跳过 '*'
    } catch (const std::exception &) {
      return "-ERR Protocol error: invalid number of elements\r\n";
    }
    pos = request.find('\n', pos) + 1; // 跳过 '\r\n'

    ASYNC_REDIS_SERVER_LOG_DEBUG("request: " << request << '\n');
    ASYNC_REDIS_SERVER_LOG_DEBUG("Number of elements: " << numElements << '\n');

    std::vector<std::string> args;

    for (int i = 0; i < numElements; ++i) {
      if (pos >= request.size() || request[pos] != '$') {
        ASYNC_REDIS_SERVER_LOG_DEBUG(
            "pos = " << pos << ", i = " << i << ", last args = "
                     << (args.empty() ? "N/A" : args.back()) << '\n');
        ASYNC_REDIS_SERVER_LOG_DEBUG("-ERR Protocol error: expected '$'\r\n");
        return "-ERR Protocol error: expected '$'\r\n";
      }

      int len = 0;
      std::string value_len;
      int next_n_pos;
      try {
        next_n_pos = request.find('\n', pos);
        len = std::stoi(request.substr(pos + 1)); // 跳过 '$'
      } catch (const std::exception &) {
        ASYNC_REDIS_SERVER_LOG_DEBUG(
            "-ERR Protocol error: invalid bulk string length\r\n");
        return "-ERR Protocol error: invalid bulk string length\r\n";
      }
      pos = next_n_pos + 1; // 跳过 '$' 值 \r\n
      if (pos + len > request.size()) {
        ASYNC_REDIS_SERVER_LOG_DEBUG(
            "-ERR Protocol error: bulk string length exceeds request size\r\n");
        return "-ERR Protocol error: bulk string length exceeds request "
               "size\r\n";
      }
      args.push_back(request.substr(pos, len));
      next_n_pos = request.find('\n', pos);
      pos = next_n_pos + 1; // 跳过数据和/r/n
    }
    ASYNC_REDIS_SERVER_LOG_DEBUG("Parsed Request: ");
    for (const auto &arg : args) {
      ASYNC_REDIS_SERVER_LOG_DEBUG(arg << " ");
    }
    ASYNC_REDIS_SERVER_LOG_DEBUG('\n');

    // 处理命令 (与 Muduo 版本相同)
    if (args.empty()) {
      return "-ERR Protocol error: empty request\r\n";
    }
    switch (string2Ops(args[0])) {
    case OPS::PING:
      return "+PONG\r\n";
    case OPS::FLUSHALL:
      return flushall_handler(redis_);
    case OPS::SAVE:
      return save_handler(redis_);
    case OPS::SET:
      return set_handler(args, redis_);
    case OPS::GET:
      return get_handler(args, redis_);
    case OPS::DEL:
      return del_handler(args, redis_);
    case OPS::INCR:
      return incr_handler(args, redis_);
    case OPS::DECR:
      return decr_handler(args, redis_);
    case OPS::EXPIRE:
      return expire_handler(args, redis_);
    case OPS::TTL:
      return ttl_handler(args, redis_);
    case OPS::HSET:
      return hset_handler(args, redis_);
    case OPS::HGET:
      return hget_handler(args, redis_);
    case OPS::HDEL:
      return hdel_handler(args, redis_);
    case OPS::HKEYS:
      return hkeys_handler(args, redis_);
    case OPS::LLEN:
      return llen_handler(args, redis_);
    case OPS::LPUSH:
      return lpush_handler(args, redis_);
    case OPS::RPUSH:
      return rpush_handler(args, redis_);
    case OPS::LPOP:
      return lpop_handler(args, redis_);
    case OPS::RPOP:
      return rpop_handler(args, redis_);
    case OPS::LRANGE:
      return lrange_handler(args, redis_);
    case OPS::ZADD:
      return zadd_handler(args, redis_);
    case OPS::ZCARD:
      return zcard_handler(args, redis_);
    case OPS::ZINCRBY:
      return zincrby_handler(args, redis_);
    case OPS::ZRANGE:
      return zrange_handler(args, redis_);
    case OPS::ZRANK:
      return zrank_handler(args, redis_);
    case OPS::ZSCORE:
      return zscore_handler(args, redis_);
    case OPS::ZREM:
      return zrem_handler(args, redis_);
    case OPS::SADD:
      return sadd_handler(args, redis_);
    case OPS::SMEMBERS:
      return smembers_handler(args, redis_);
    case OPS::SCARD:
      return scard_handler(args, redis_); // SISMEMBER 的 args[0] 是 SCARD
    case OPS::SISMEMBER:
      return sismember_handler(args, redis_);
    case OPS::SREM:
      return srem_handler(args, redis_);
    default:
      return "-ERR unknown command '" + args[0] + "'\r\n";
    }
  }

  tcp::socket socket_;
  asio::streambuf buffer_; // 使用 streambuf 处理读入的数据
  RedisWrapper &redis_;    // 数据库实例，通过引用传递
};

class RedisServer {
public:
  RedisServer(asio::io_context &io_context, unsigned short port)
      : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)),
        redis_("example_db") { // 初始化 RedisWrapper
    do_accept();
  }

private:
  void do_accept() {
    acceptor_.async_accept(
        [this](const asio::error_code &ec, tcp::socket socket) {
          if (!ec) {
            std::make_shared<RedisSession>(std::move(socket), redis_)->start();
          } else {
            ASYNC_REDIS_SERVER_LOG_INFO("Error on accept: " << ec.message());
          }
          do_accept(); // 继续接受新的连接
        });
  }

  tcp::acceptor acceptor_;
  RedisWrapper redis_; // 数据库实例
};

int main() {
  try {
    asio::io_context io_context;
    g_io_context = &io_context;
    // 注册信号
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    RedisServer server(io_context, 6380); // Redis 默认端口
    io_context.run();                     // 运行事件循环
  } catch (std::exception &e) {
    std::cerr << "Exception: " << e.what() << std::endl;
  }

  return 0;
}