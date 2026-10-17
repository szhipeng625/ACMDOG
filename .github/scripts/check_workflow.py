import sys
import yaml

path = sys.argv[1]
with open(path, "r", encoding="utf-8") as fp:
    data = yaml.safe_load(fp)
jobs = data.get("jobs", {})
steps = sum(len(j.get("steps", [])) for j in jobs.values())
print("workflow ok:", path, "jobs:", len(jobs), "steps:", steps)
