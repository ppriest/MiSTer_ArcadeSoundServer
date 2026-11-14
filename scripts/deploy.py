#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the sound server (src/) for the MiSTer and install it.

    python scripts/deploy.py [--no-install] [--package]

Cross-compiles with WSL's arm-linux-gnueabihf-gcc/g++ (static). Installs
/media/fat/games/arcsnd/arcsnd; Scripts/arcade_hybridsoundserver.sh, which starts or stops it from the Scripts menu
(`start` at boot from linux/user-startup.sh). --package writes releases/arcsnd_<YYYYMMDD>.zip, the files where they go under
/media/fat, arcsnd.ini included (unpacking it over an install replaces the user's). deploy.py itself installs
games/arcsnd/arcsnd.ini only when there is none.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "src"
OBJ = REPO / "obj" / "arm"
# unwind tables in the C as well, and line tables: the crash report (common/daemon.cpp) walks the
# stack with the first, addr2line resolves it with the second
ARCH = "-O3 -g -marm -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -funwind-tables"
# -Icommon/miniz and the Model 3 sources below were added with the Model 3 board: miniz reads its sound ROMs
# from the set's zip, dsb_m68k.c is its Digital Sound Board's, and common/m68k_bus.cpp holds Musashi's
# memory callbacks for both 68000 boards
CXX = f"-std=c++17 {ARCH} -Wall -Wno-sign-compare -Icommon -Icommon/miniz"
C_SRC = ["common/cpus/musashi/m68kcpu.c", "common/cpus/musashi/m68kops.c", "common/cpus/musashi/m68kdasm.c",
         "common/cpus/musashi/softfloat/softfloat.c", "common/miniz/miniz.c", "model3/dsb_m68k.c"]
CXX_SRC = ["server.cpp", "common/daemon.cpp", "common/logerror.cpp", "common/m68k_bus.cpp", "common/config.cpp",
           "hornet/board.cpp", "hornet/rf5c400.cpp", "hornet/session.cpp",
           "model3/board.cpp", "model3/SCSP.cpp", "model3/SCSPDSP.cpp", "model3/scspdsp_jit.cpp", "model3/dsplink_shm.cpp", "model3/roms.cpp", "model3/session.cpp",
           "model3/dsb.cpp", "common/cpus/z80/Z80.cpp",
           "hng64/nec_core.cpp", "hng64/v53.cpp", "hng64/l7a1045.cpp", "hng64/board.cpp", "hng64/session.cpp",
           "firebeat/synth.cpp", "firebeat/session.cpp"]
# Keyboardmania's SoundFonts, installed beside arcsnd: the XG set arcsnd.ini names, and TimGM6mb when it
# is not there (src/firebeat/session.cpp's order)
SF2S = [REPO / "assets" / "Yamaha XG Sound Set.sf2", REPO / "assets" / "TimGM6mb.sf2"]
# installed when there is none, so a user's edits stand
INI = REPO / "assets" / "arcsnd.ini"
REMOTE_DIR = "/media/fat/games/arcsnd"
REMOTE_START = "/media/fat/Scripts/arcade_hybridsoundserver.sh"

START = """#!/bin/sh
# arcsnd: arcade sound boards on the ARM (Konami Hornet, Hyper NeoGeo 64, Sega Model 3), in the background. It
# waits for a set it knows, goes quiet at every core load, and runs as one copy.
#   (no argument)  from the Scripts menu: stop it if it is running, else start it
#   start          start it if it is not running (linux/user-startup.sh's $1 at boot)
#   stop           stop it
DIR={dir}
LOG=/tmp/arcsnd.log
running() {{ pidof arcsnd >/dev/null; }}
launch() {{ cd "$DIR" || exit 1; chmod +x arcsnd; setsid ./arcsnd </dev/null >/dev/null 2>"$LOG" & }}
halt() {{ kill $(pidof arcsnd); sleep 1; if running; then echo "arcsnd did not stop"; exit 1; fi; echo "arcsnd stopped"; }}
case "$1" in
start) running || launch ;;
stop) if running; then halt; fi ;;
*)
	if running; then echo "arcsnd is running: stopping it"; halt; exit 0; fi
	echo "starting arcsnd"; launch; sleep 5
	if running; then echo "arcsnd started:"; cat "$LOG"; else echo "arcsnd failed to start:"; cat "$LOG"; exit 1; fi
	;;
esac
"""


def load_env():
    """MISTER_HOST, MISTER_USER, MISTER_PASSWORD: the environment, then this repository's mister.env
    (gitignored), then the per-machine file the cores share (MISTER_CORE_ENV, else
    ~/.mister-core.env)."""
    env = {}
    for path in (Path(os.environ.get("MISTER_CORE_ENV") or Path.home() / ".mister-core.env"), REPO / "mister.env"):
        if path.exists():
            for ln in path.read_text(encoding="utf-8", errors="replace").splitlines():
                ln = ln.strip()
                if ln and not ln.startswith("#") and "=" in ln:
                    k, v = ln.split("=", 1)
                    env[k.strip()] = v.strip().strip('"').strip("'")
    for k in ("MISTER_HOST", "MISTER_USER", "MISTER_PASSWORD"):
        if os.environ.get(k):
            env[k] = os.environ[k]
        if not env.get(k):
            sys.exit(f"{k} is not set (environment, mister.env or ~/.mister-core.env)")
    return env


def find_putty(name):
    for c in (name, os.path.join(r"C:\Program Files\PuTTY", name)):
        found = shutil.which(c) or (c if os.path.isfile(c) else None)
        if found:
            return found
    sys.exit(f"Couldn't find {name}. Install PuTTY, or put it on PATH.")


class Mister:
    def __init__(self, env):
        self.host, self.user, self.pw = env["MISTER_HOST"], env["MISTER_USER"], env["MISTER_PASSWORD"]
        self.plink, self.pscp = find_putty("plink.exe"), find_putty("pscp.exe")

    def sh(self, command, timeout=60, check=True):
        p = subprocess.run([self.plink, "-ssh", "-batch", "-pw", self.pw, f"{self.user}@{self.host}", command],
                           capture_output=True, text=True, timeout=timeout)
        if check and p.returncode != 0:
            sys.exit(f"ssh failed ({p.returncode}): {command}\n{p.stderr.strip()}")
        return p.stdout


def wsl_path(p):
    p = Path(p).resolve()
    return f"/mnt/{p.drive[0].lower()}{p.as_posix()[2:]}"


def wsl(cmds):
    r = subprocess.run(["wsl.exe", "-e", "bash", "-lc", " && ".join(cmds)], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"build failed:\n{r.stdout}{r.stderr}")


def build_id():
    """The commit, -dirty when the tracked files differ from it."""
    git = lambda *a: subprocess.run(["git", "-C", str(REPO), *a], capture_output=True, text=True).stdout.strip()
    rev = git("rev-parse", "--short", "HEAD") or "unknown"
    return rev + ("-dirty" if git("status", "--porcelain", "--untracked-files=no") else "")


def build(out):
    o = wsl_path(OBJ)
    bid = build_id()
    (OBJ / "arcsnd_build.h").write_text(f'#define ARCSND_BUILD "{bid}"\n'
                                        f'#define ARCSND_DATE "{time.strftime("%Y/%m/%d")}"\n')
    cmds = [f"cd {wsl_path(SRC)}", f"mkdir -p {o}"]
    objs = []
    for s in C_SRC:
        obj = f"{o}/{Path(s).stem}.o"
        cmds.append(f"arm-linux-gnueabihf-gcc {ARCH} -Icommon/cpus/musashi -c {s} -o {obj}")
        objs.append(obj)
    for s in CXX_SRC:
        obj = f"{o}/{s.replace('/', '_')[:-4]}.o"
        cmds.append(f"arm-linux-gnueabihf-g++ {CXX} -I{o} -c {s} -o {obj}")
        objs.append(obj)
    cmds.append(f"arm-linux-gnueabihf-g++ {ARCH} -static -o {wsl_path(out)} " + " ".join(objs))
    wsl(cmds)
    # each build kept by its commit, with its line tables: a crash report from that build is resolved
    # against it. What is installed and packaged is stripped (the same addresses, a third the size)
    (OBJ / "builds").mkdir(exist_ok=True)
    shutil.copyfile(out, OBJ / "builds" / f"arcsnd_{bid}")
    wsl([f"arm-linux-gnueabihf-strip {wsl_path(out)}"])


def put(m, local, remote):
    p = subprocess.run([m.pscp, "-batch", "-pw", m.pw, str(local), f"{m.user}@{m.host}:{remote}"],
                       capture_output=True, text=True, timeout=600)
    if p.returncode != 0:
        sys.exit(f"upload failed: {local}\n{p.stderr.strip()}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--package", action="store_true", help="write releases/arcsnd_<YYYYMMDD>.zip")
    ap.add_argument("--no-install", action="store_true", help="build (and package) only")
    a = ap.parse_args()
    OBJ.mkdir(parents=True, exist_ok=True)
    binary = OBJ / "arcsnd"
    build(binary)
    start = OBJ / Path(REMOTE_START).name
    start.write_bytes(START.format(dir=REMOTE_DIR).encode())
    if a.package:
        out = REPO / "releases" / f"arcsnd_{time.strftime('%Y%m%d')}.zip"
        with zipfile.ZipFile(out, "w") as z:
            files = [(REMOTE_DIR + "/arcsnd", binary.read_bytes()), (REMOTE_START, start.read_bytes())]
            files += [(REMOTE_DIR + "/" + f.name, f.read_bytes()) for f in SF2S]
            files.append((REMOTE_DIR + "/" + INI.name, INI.read_bytes()))
            for path, data in files:
                zi = zipfile.ZipInfo(path.removeprefix("/media/fat/"), time.localtime()[:6])
                zi.create_system = 3
                zi.external_attr = (0o100644 if path.endswith((".sf2", ".ini")) else 0o100755) << 16
                zi.compress_type = zipfile.ZIP_DEFLATED
                z.writestr(zi, data)
                # ... and the same files unpacked under releases/ in the SD card's layout, for a
                # downloader database to fetch one by one
                f = REPO / "releases" / zi.filename
                f.parent.mkdir(parents=True, exist_ok=True)
                f.write_bytes(data)
        print(f"-> {out} and releases/{REMOTE_DIR.removeprefix('/media/fat/')}/arcsnd, releases/{REMOTE_START.removeprefix('/media/fat/')}")
    if a.no_install:
        print(f"-> {binary}")
        return 0
    m = Mister(load_env())
    m.sh(f"mkdir -p {REMOTE_DIR} {Path(REMOTE_START).parent.as_posix()}")
    m.sh("for p in $(ps | grep '[a]rcsnd' | awk '{print $1}'); do kill $p; done; sleep 1; true", check=False)
    put(m, binary, f"{REMOTE_DIR}/arcsnd")
    m.sh(f"chmod +x {REMOTE_DIR}/arcsnd")
    put(m, start, REMOTE_START)
    for f in SF2S:
        put(m, f, f"{REMOTE_DIR}/{f.name}")
    if m.sh(f"test -e {REMOTE_DIR}/{INI.name} || echo none").strip() == "none":
        put(m, INI, f"{REMOTE_DIR}/{INI.name}")
    m.sh(f"chmod +x {REMOTE_START}")
    print(f"-> {REMOTE_DIR}/arcsnd, {REMOTE_START}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
