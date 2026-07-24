#!/usr/bin/env python3
import argparse, os, signal, subprocess, sys, time

p = argparse.ArgumentParser()
p.add_argument('--timeout-sec', type=int, required=True)
p.add_argument('--log', required=True)
p.add_argument('command', nargs=argparse.REMAINDER)
a = p.parse_args()
if a.command and a.command[0] == '--': a.command = a.command[1:]
if not a.command: sys.exit(2)
with open(a.log, 'w', encoding='utf-8') as out:
    proc = subprocess.Popen(a.command, stdout=out, stderr=subprocess.STDOUT,
                            start_new_session=True)
    try:
        rc = proc.wait(timeout=a.timeout_sec)
    except subprocess.TimeoutExpired:
        out.write(f'\n[netem-report] case watchdog timeout after {a.timeout_sec}s; terminating process group\n')
        out.flush()
        os.killpg(proc.pid, signal.SIGTERM)
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
        rc = 124
sys.exit(rc)
