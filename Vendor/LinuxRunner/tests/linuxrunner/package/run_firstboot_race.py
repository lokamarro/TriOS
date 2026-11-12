#!/usr/bin/env python3
"""Host driver for firstboot_race.sh on freshly unpacked copies of the shipped
Debian image (the same packages and first-start scripts the iPhone app installs).

  run_firstboot_race.py --ish ISH --fakefsify FAKEFSIFY --image debian-shipped.tar
                        --work NEW_EMPTY_DIR --exchange DIR --gui REL_GUI_DIR

Cases: "race" (init.sh and the package lock together) and "resume" (first start
killed after a few seconds, as when iOS ends the app, then the lock is taken
with nothing running the configuration). Each case gets its own fresh image in
--work; nothing outside --work is changed. --exchange is bind-mounted at
/home/mobile/Downloads and must contain firstboot_race.sh and the support
folder REL_GUI_DIR (package-lock.sh)."""
import argparse, os, signal, subprocess, sys, time

p = argparse.ArgumentParser()
for a in ("ish", "fakefsify", "image", "work", "exchange", "gui"):
    p.add_argument("--" + a, required=True)
p.add_argument("--interrupt-after", type=float, default=8.0)
p.add_argument("--timeout", type=float, default=1800.0)
args = p.parse_args()

if os.path.exists(args.work) and os.listdir(args.work):
    sys.exit(f"--work {args.work} must be new or empty")
os.makedirs(args.work, exist_ok=True)
env = dict(os.environ, ISH_NETLINK_STUB="1", ISH_BIND_MOUNTS=f"/home/mobile/Downloads={args.exchange}")
gui = "/home/mobile/Downloads/" + args.gui


def ish(root, *cmd, timeout=args.timeout, bind=True):
    return subprocess.run([args.ish, "-f", root, *cmd], env=env if bind else None,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout, text=True, errors="replace")


NOISE = ("HOST_RESOURCES", "PROCESS_EVENT", "GUEST_BOOT", "fakefs_bind_mount", "create_relative_symlink")


def guest_lines(text):
    return [l for l in text.split("\n") if l and not l.startswith(NOISE)]


failed = 0
for case in ("race", "resume"):
    root = os.path.join(args.work, case)
    subprocess.run([args.fakefsify, args.image, root], check=True, stdout=subprocess.DEVNULL)
    ish(root, "/bin/mkdir", "-p", "/home/mobile/Downloads", bind=False, timeout=120)
    if case == "resume":
        proc = subprocess.Popen([args.ish, "-f", root, "/bin/sh", "/usr/lib/linuxrunner/init.sh"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        time.sleep(args.interrupt_after)
        os.killpg(proc.pid, signal.SIGKILL); proc.wait()
        state = ish(root, "/bin/sh", "-c", "grep -c '^Setting up ' /var/log/linuxrunner-initialize.log; "
                    "ls /var/lib/linuxrunner; dpkg --audit 2>&1 | head -n 3", bind=False, timeout=300).stdout
        print(f"[resume] after interrupt at {args.interrupt_after}s: " + " | ".join(guest_lines(state)[:5]))
    out = ish(root, "/bin/sh", "/home/mobile/Downloads/firstboot_race.sh", case, gui)
    print(f"[{case}] exit={out.returncode}"); print("\n".join(guest_lines(out.stdout)))
    if f"FIRSTBOOT_RACE_TEST {case} PASS" not in out.stdout:
        failed += 1
print(f"FIRSTBOOT_RACE {'PASS' if not failed else 'FAIL'} failed_cases={failed}")
sys.exit(1 if failed else 0)
