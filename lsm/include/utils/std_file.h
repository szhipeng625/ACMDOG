#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tiny_lsm {
class StdFile {

private:
#ifdef _WIN32
  // 不透明句柄：Windows 下为 HANDLE（void*），避免在头文件引入 windows.h
  void *handle_ = reinterpret_cast<void *>(static_cast<intptr_t>(-1));
#else
  int fd_ = -1;
#endif
  std::filesystem::path filename_;

public:
  StdFile() = default;
  ~StdFile() {
    if (is_open()) {
      close();
    }
  }

  bool is_open() const {
#ifdef _WIN32
    return handle_ != reinterpret_cast<void *>(static_cast<intptr_t>(-1));
#else
    return fd_ >= 0;
#endif
  }

  // 打开文件并映射到内存
  bool open(const std::string &filename, bool create);

  // 创建文件
  bool create(const std::string &filename, std::vector<uint8_t> &buf);

  // 关闭文件
  void close();

  // 获取文件大小
  size_t size();

  // 写入数据
  bool write(size_t offset, const void *data, size_t size);

  // 读取数据
  std::vector<uint8_t> read(size_t offset, size_t length);

  // 同步到磁盘
  bool sync();

  // 删除文件
  bool remove();

  bool truncate(size_t size);
};
} // namespace tiny_lsm
