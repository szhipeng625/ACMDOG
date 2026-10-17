import sys
import yaml

for f in sys.argv[1:]:
    with open(f, "r", encoding="utf-8") as fp:
        yaml.safe_load(fp)
    print("yaml ok:", f)
