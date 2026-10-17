import sys, tomllib

with open(sys.argv[1], "rb") as fp:
    data = tomllib.load(fp)
print("config ok:", sys.argv[1], "keys:", len(data))
