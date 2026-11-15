#!/usr/bin/env python3
"""Streaming-capture acceptance matrix (plan doc Appendix C.7).

Runs the V1-V10 cases against the demo driver; V10 (push to GUI) is
skipped when no GUI instance is listening. Exit code 0 = all pass.

Usage: python3 tools/u3ctl/tests/test_streaming.py [path-to-u3ctl]
"""

import json
import os
import signal
import subprocess
import sys
import time

U3CTL = sys.argv[1] if len(sys.argv) > 1 else "./build_u3ctl/u3ctl"
JOB_DIR = "captures"

passed = failed = skipped = 0


def run(args, timeout=30):
    return subprocess.run([U3CTL] + args, capture_output=True, text=True,
                          timeout=timeout)


def jrun(args, timeout=30):
    p = run(args, timeout)
    try:
        return json.loads(p.stdout), p.returncode
    except json.JSONDecodeError:
        return {"_raw": p.stdout, "_err": p.stderr}, p.returncode


def check(name, ok, detail=""):
    global passed, failed
    if ok:
        passed += 1
        print(f"  PASS {name}")
    else:
        failed += 1
        print(f"  FAIL {name} {detail}")


def wait_terminal(job, limit=30):
    for _ in range(int(limit / 0.2)):
        d, _ = jrun(["capture-status", "--job", job])
        if d.get("status") in ("done", "failed", "interrupted"):
            return d
        time.sleep(0.2)
    return d


def cleanup_jobs():
    subprocess.run([U3CTL, "capture-list"], capture_output=True)


print("== V1: capture-start returns in < 1s ==")
t0 = time.time()
d, rc = jrun(["capture-start", "--driver", "demo", "--samples", "200K",
              "--rate", "1M", "--out", "captures/v1.sr", "--overwrite",
              "--demo-pattern", "random"])
dt = time.time() - t0
check("V1 non-blocking", rc == 0 and "job" in d and dt < 1.0,
      f"rc={rc} dt={dt:.2f} {d}")
v1_job = d.get("job", "")
wait_terminal(v1_job)

print("== V2: auto-stop at --samples, files agree ==")
d, rc = jrun(["capture-start", "--driver", "demo", "--samples", "100K",
              "--rate", "1M", "--out", "captures/v2.sr", "--overwrite",
              "--demo-pattern", "random"])
job = d.get("job", "")
st = wait_terminal(job)
check("V2 status done at limit",
      st.get("status") == "done" and st.get("samples") == 100000,
      f"{st}")
info, _ = jrun(["session", "info", "captures/v2.sr"])
check("V2 .sr sample count", info.get("total_samples") == 100000, f"{info}")
res, _ = jrun(["capture-stop", "--job", job])  # terminal → error per C.5
check("V2 stop-after-done is explicit error",
      "error" in res, f"{res}")

print("== V3: graceful stop mid-capture ==")
d, rc = jrun(["capture-start", "--driver", "demo", "--rate", "1M",
              "--out", "captures/v3.sr", "--overwrite",
              "--demo-pattern", "random", "--timeout", "600"])
job3 = d.get("job", "")
s1, _ = jrun(["capture-status", "--job", job3])
time.sleep(1.0)
s2, _ = jrun(["capture-status", "--job", job3])
check("V3 status samples increase while running",
      s1.get("samples", 0) < s2.get("samples", 0) and s2.get("samples", 0) > 0,
      f"{s1.get('samples')} -> {s2.get('samples')}")
res, rc = jrun(["capture-stop", "--job", job3], timeout=15)
check("V3 stop returns capture-shaped JSON",
      rc == 0 and res.get("samples", 0) > 0 and res.get("job") == job3
      and "samplerate" in res, f"rc={rc} {res}")
info, _ = jrun(["session", "info", "captures/v3.sr"])
check("V3 stopped .sr is readable", info.get("total_samples", 0) > 0, f"{info}")

print("== V4: kill -9 worker → interrupted ==")
d, _ = jrun(["capture-start", "--driver", "demo", "--rate", "1M",
             "--out", "captures/v4.sr", "--overwrite",
             "--demo-pattern", "random", "--timeout", "600"])
job4 = d.get("job", "")
time.sleep(0.8)
os.kill(d["pid"], signal.SIGKILL)
time.sleep(0.3)
st, _ = jrun(["capture-status", "--job", job4])
check("V4 interrupted after crash",
      st.get("status") == "interrupted" and st.get("pid_alive") is False
      and "died" in st.get("note", ""), f"{st}")

print("== V5: forged orphan state ==")
now = int(time.time() * 1000)
forged = {
    "id": "job-forged", "status": "running", "out": "captures/v5.sr",
    "pid": 999999, "started_ms": now, "updated_ms": now,
    "samplerate": 1, "samples": 42, "trigger_seen": False,
    "trigger_pos": 0, "will_auto_stop": False, "note": "",
}
with open(f"{JOB_DIR}/.job-job-forged.json.tmp", "w") as f:
    json.dump(forged, f)
os.rename(f"{JOB_DIR}/.job-job-forged.json.tmp",
          f"{JOB_DIR}/.job-job-forged.json")
st, _ = jrun(["capture-status", "--job", "job-forged"])
check("V5 orphan reported honestly",
      st.get("status") == "interrupted" and st.get("pid_alive") is False,
      f"{st}")

print("== V6: trigger passes through the worker ==")
d, _ = jrun(["capture-start", "--driver", "demo", "--samples", "100K",
             "--rate", "1M", "--out", "captures/v6.sr", "--overwrite",
             "--demo-pattern", "random", "--trigger", "D0:rising"])
st = wait_terminal(d.get("job", ""))
res, _ = jrun(["capture-status", "--job", d.get("job", "")])
check("V6 trigger seen", st.get("trigger_seen") is True, f"{st}")

print("== V7: second start while one runs → busy ==")
d, _ = jrun(["capture-start", "--driver", "demo", "--rate", "1M",
             "--out", "captures/v7a.sr", "--overwrite",
             "--demo-pattern", "random", "--timeout", "600"])
time.sleep(0.5)
d2, rc2 = jrun(["capture-start", "--driver", "demo", "--rate", "1M",
                "--out", "captures/v7b.sr", "--overwrite",
                "--demo-pattern", "random"])
check("V7 busy rejection",
      rc2 == 2 and "busy" in d2.get("error", ""), f"rc={rc2} {d2}")
jrun(["capture-stop", "--job", d.get("job", "")])

print("== V8: stop edge cases ==")
d, rc = jrun(["capture-stop", "--job", "job-does-not-exist"])
check("V8 stop unknown job", rc == 2 and "error" in d, f"rc={rc} {d}")

print("== V9: capture-list sees everything ==")
d, _ = jrun(["capture-list"])
ids = {j["job"] for j in d.get("jobs", [])}
need = {v1_job, job3, job4, "job-forged"}
check("V9 list contains jobs incl interrupted",
      need.issubset(ids), f"missing {need - ids}")

print("== V10: stop --open pushes to GUI ==")
sock = f"/tmp/u3la-open-{os.getuid()}.sock"
if os.path.exists(sock):
    d, _ = jrun(["capture-start", "--driver", "demo", "--rate", "1M",
                 "--out", "captures/v10.sr", "--overwrite",
                 "--demo-pattern", "random"])
    time.sleep(0.5)
    res, rc = jrun(["capture-stop", "--job", d.get("job", ""), "--open"])
    check("V10 stop --open ok", rc == 0 and res.get("samples", 0) > 0, f"{res}")
else:
    skipped += 1
    print("  SKIP V10 (no GUI instance listening)")

print(f"\n=== {passed} passed, {failed} failed, {skipped} skipped ===")
sys.exit(1 if failed else 0)
