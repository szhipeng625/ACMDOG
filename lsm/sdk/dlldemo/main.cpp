// 独立外部调用示例：以“动态链接”方式使用 lsm_shared.dll
// - 编译时定义 TINYLSM_USE_DLL，头文件中的 TINYLSM_API 展开为 __declspec(dllimport)
// - 链接 lsm_shared.lib 导入库，运行时依赖 lsm_shared.dll
// 该文件不接触项目内部源码，只使用 include/ 公共头 + 导入库，用于验证 DLL 确实可被外部调用。
#define TINYLSM_USE_DLL
#include "lsm/engine.h"
#include "lsm/level_iterator.h" // engine.h 仅前向声明 Level_Iterator，使用迭代器需包含完整定义

#include <iostream>

int main() {
  using namespace tiny_lsm;

  LSM db("dlldemo_data");

  // 基础写 / 读
  db.put("hello", "world");
  db.put("foo", "bar");
  std::cout << "[get] hello -> " << db.get("hello").value_or("<none>") << "\n";
  std::cout << "[get] missing has_value = " << db.get("nope").has_value()
            << "\n";

  // 删除
  db.remove("foo");
  std::cout << "[remove] foo has_value = " << db.get("foo").has_value()
            << "\n";

  // 范围迭代（begin(0) 表示关闭 MVCC，读到全部可见键）
  db.put("k1", "v1");
  db.put("k2", "v2");
  int n = 0;
  for (auto it = db.begin(0); it != db.end(); ++it) {
    ++n;
  }
  std::cout << "[iterator] visible keys = " << n << "\n";

  // 事务：提交后应可读到
  auto tx = db.begin_tran(IsolationLevel::SERIALIZABLE);
  tx->put("txkey", "txval");
  bool committed = tx->commit();
  std::cout << "[txn] commit=" << committed
            << " txkey=" << db.get("txkey").value_or("<none>") << "\n";

  db.clear();
  std::cout << "DLL-DEMO OK\n";
  return 0;
}
