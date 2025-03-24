import sys
import os
from glob import glob

def get_gen_event(path):
    temp=""
    injection_line=""
    with open(path, "r") as f:
        for line in f.readlines():
            if len(line.split()) > 1 and line.split()[-2] == "G---":
                return line.split()[-3]

equivalence_classes={}
for x in glob(f"./**/*.event_trace", recursive=True):
    basename = x[:x.rfind(".")]
    key = get_gen_event(x)
    # value = (get_injection_line_and_events_trace_hash(basename+".event_trace")[0], basename+".event_trace", basename+".out")
    # value = tuple([os.path.relpath(x) for x in value])
    value = x
    if key not in equivalence_classes:
        equivalence_classes[key] = [value]
    else:
        equivalence_classes[key].append(value)

with open("equivalence_classes.txt", "w") as f:
    for i, x in enumerate(sorted(equivalence_classes.items(), key=lambda x:x[1])):
        print(f"=============== {i+1} ===============", file=f)
        for y in x[1]:
            print(f"  {y}", file=f)
        print("",file=f)