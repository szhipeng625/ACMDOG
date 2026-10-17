#include "utils/std_file.h"
#include "spdlog/spdlog.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tiny_lsm {

#ifdef _WIN32
// Windows 实现：CreateFile + ReadFile/WriteFile + OVERLAPPED（位置读写）。
// OVERLAPPED 显式携带偏移，不更新文件指针，同一句柄上并发调用安全，
// 语义与 POSIX 的 pread/pwrite 对齐。open 时带 FILE_SHARE_DELETE，
// 允许在句柄仍打开时删除文件（对应 POSIX 的 unlink）。

namespace {
// reinterpret_cast 在 MSVC 下不能用于 constexpr 常量表达式，改用 void* const
void *const kInvalidHandle = reinterpret_cast<void *>(static_cast<intptr_t>(-1));
}

bool StdFile::open(const std::string &filename, bool create) {
  filename_ = filename;

  HANDLE h = CreateFileA(
      filename.c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      create ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

  if (h == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    std::string error_msg =
        fmt::format("Failed to open file '{}': error code {}", filename, err);

    spdlog::error(error_msg);
    std::cerr << "[ERROR] " << error_msg << std::endl;
    return false;
  }

  handle_ = h;
  return true;
}

bool StdFile::create(const std::string &filename, std::vector<uint8_t> &buf) {
  if (!this->open(filename, true)) {
    throw std::runtime_error("Failed to open file for writing");
  }
  if (!buf.empty()) {
    write(0, buf.data(), buf.size());
  }

  return true;
}

void StdFile::close() {
  if (is_open()) {
    sync();
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = kInvalidHandle;
  }
}

size_t StdFile::size() {
  if (!is_open()) {
    return 0;
  }
  LARGE_INTEGER sz;
  if (!GetFileSizeEx(static_cast<HANDLE>(handle_), &sz)) {
    return 0;
  }
  return static_cast<size_t>(sz.QuadPart);
}

std::vector<uint8_t> StdFile::read(size_t offset, size_t length) {
  std::vector<uint8_t> buf(length);
  size_t done = 0;
  while (done < length) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset + done);
    ov.OffsetHigh = static_cast<DWORD>((offset + done) >> 32);
    DWORD n = 0;
    if (!ReadFile(static_cast<HANDLE>(handle_), buf.data() + done,
                  static_cast<DWORD>(length - done), &n, &ov)) {
      // EOF（ERROR_HANDLE_EOF）或其它错误统一按读取失败处理，保持原异常语义
      throw std::runtime_error("Failed to read from file");
    }
    if (n == 0) {
      throw std::runtime_error("Failed to read from file");
    }
    done += n;
  }
  return buf;
}

bool StdFile::write(size_t offset, const void *data, size_t size) {
  if (!is_open()) {
    return false;
  }
  const uint8_t *ptr = static_cast<const uint8_t *>(data);
  size_t done = 0;
  while (done < size) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset + done);
    ov.OffsetHigh = static_cast<DWORD>((offset + done) >> 32);
    DWORD n = 0;
    if (!WriteFile(static_cast<HANDLE>(handle_), ptr + done,
                   static_cast<DWORD>(size - done), &n, &ov)) {
      return false;
    }
    if (n == 0) {
      return false;
    }
    done += n;
  }
  return true;
}

bool StdFile::sync() {
  if (!is_open()) {
    return false;
  }
  return FlushFileBuffers(static_cast<HANDLE>(handle_)) != 0;
}

bool StdFile::remove() {
  // Windows 下用 DeleteFileW：open 时已带 FILE_SHARE_DELETE，可删除被打开的文件
  return DeleteFileW(filename_.c_str()) != 0;
}

bool StdFile::truncate(size_t size) {
  if (!is_open()) {
    return false;
  }
  LARGE_INTEGER pos;
  pos.QuadPart = static_cast<LONGLONG>(size);
  if (!SetFilePointerEx(static_cast<HANDLE>(handle_), pos, nullptr,
                        FILE_BEGIN)) {
    return false;
  }
  return SetEndOfFile(static_cast<HANDLE>(handle_)) != 0;
}

#else
// POSIX 实现（Linux/macOS/BSD）：open + pread/pwrite + fstat。
// pread/pwrite 每次调用显式携带偏移，不修改文件偏移状态，
// 同一 fd 上多线程并发读写是安全的（修复 fstream 位置竞争）。

bool StdFile::open(const std::string &filename, bool create) {
  filename_ = filename;

  int flags = O_RDWR;
  if (create) {
    flags |= O_CREAT | O_TRUNC;
  }
  fd_ = ::open(filename.c_str(), flags, 0644);

  if (fd_ < 0) {
    // 获取具体的错误信息
    int err = errno;
    std::string error_msg = fmt::format("Failed to open file '{}': {} ({})",
                                        filename, strerror(err), err);

    // 同时输出到日志文件和标准错误
    spdlog::error(error_msg);
    std::cerr << "[ERROR] " << error_msg << std::endl;

    return false;
  }

  return true;
}

bool StdFile::create(const std::string &filename, std::vector<uint8_t> &buf) {
  if (!this->open(filename, true)) {
    throw std::runtime_error("Failed to open file for writing");
  }
  if (!buf.empty()) {
    write(0, buf.data(), buf.size());
  }

  return true;
}

void StdFile::close() {
  if (fd_ >= 0) {
    sync();
    ::close(fd_);
    fd_ = -1;
  }
}

size_t StdFile::size() {
  if (fd_ < 0) {
    return 0;
  }
  struct stat st;
  if (::fstat(fd_, &st) != 0) {
    return 0;
  }
  return static_cast<size_t>(st.st_size);
}

std::vector<uint8_t> StdFile::read(size_t offset, size_t length) {
  std::vector<uint8_t> buf(length);
  size_t done = 0;
  while (done < length) {
    ssize_t n = ::pread(fd_, buf.data() + done, length - done,
                        static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error("Failed to read from file");
    }
    if (n == 0) {
      // EOF：实际读到的字节数不足，保持原 fstream 读不满即失败的语义
      throw std::runtime_error("Failed to read from file");
    }
    done += static_cast<size_t>(n);
  }
  return buf;
}

bool StdFile::write(size_t offset, const void *data, size_t size) {
  if (fd_ < 0) {
    return false;
  }
  const uint8_t *ptr = static_cast<const uint8_t *>(data);
  size_t done = 0;
  while (done < size) {
    ssize_t n = ::pwrite(fd_, ptr + done, size - done,
                         static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (n == 0) {
      return false;
    }
    done += static_cast<size_t>(n);
  }
  return true;
}

bool StdFile::sync() {
  if (fd_ < 0) {
    return false;
  }
  return ::fsync(fd_) == 0;
}

bool StdFile::remove() {
  // 修复类型转换问题
  return std::remove((const char *)filename_.c_str()) == 0;
}

bool StdFile::truncate(size_t size) {
  if (fd_ < 0) {
    return false;
  }
  return ::ftruncate(fd_, static_cast<off_t>(size)) == 0;
}

#endif
} // namespace tiny_lsm
