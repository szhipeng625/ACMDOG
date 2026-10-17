import os
import sys

log = sys.argv[1]
out = os.environ.get("GITHUB_STEP_SUMMARY", "tests/out/summary.md")
rows = []
if os.path.exists(log):
    with open(log, "r", encoding="utf-8") as fp:
        for line in fp:
            parts = [p.strip() for p in line.strip().split("|")]
            if len(parts) >= 5 and parts[1] and parts[2] in ("PASS", "FAIL"):
                rows.append(parts[:5])
lines = []
lines.append("### ACMDOG Gate 运行记录")
lines.append("")
lines.append("提交: " + os.environ.get("GITHUB_SHA", "local"))
lines.append("")
lines.append("| 时间 | 阶段 | 结果 | 耗时 | 依据 |")
lines.append("| --- | --- | --- | --- | --- |")
passed = 0
failed = 0
for r in rows:
    status = r[2]
    if status == "PASS":
        passed += 1
    else:
        failed += 1
    detail = r[4].replace("|", "/")[:80]
    lines.append("| " + r[0] + " | " + r[1] + " | " + ("✅ " if status == "PASS" else "❌ ") + status + " | " + r[3] + " | " + detail + " |")
lines.append("")
lines.append("合计: " + str(len(rows)) + " 项，通过 " + str(passed) + "，失败 " + str(failed))
text = "\n".join(lines)
with open(out, "w", encoding="utf-8") as fp:
    fp.write(text)
print(text)
sys.exit(0 if failed == 0 else 1)
