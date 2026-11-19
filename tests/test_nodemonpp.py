#!/usr/bin/env python3
"""nodemonpp end-to-end test: a throw-away CMake app, real edits, real rebuilds, real restarts."""
import os, queue, re, shutil, signal, subprocess, sys, tempfile, threading, time

NODEMONPP = os.path.abspath(sys.argv[1])
passed = failed = 0


def check(cond, name):
    global passed, failed
    if cond:
        passed += 1
        print("  ok:", name)
    else:
        failed += 1
        print("  FAIL:", name)


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    # a zombie is not "alive" for our purpose
    try:
        with open(f"/proc/{pid}/stat") as f:
            return f.read().split()[2] != "Z"
    except FileNotFoundError:
        return False


proj = tempfile.mkdtemp(prefix="nodemonpp-test-")
os.makedirs(f"{proj}/src")


def write(rel, text):
    path = os.path.join(proj, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.16)\nproject(demo CXX)\n"
      "# a comment add_executable(wrong x)\nadd_executable(demoapp src/main.cc)\n")
write("src/version.hpp", '#define VERSION "v1"\n')
write("src/main.cc", r'''
#include "version.hpp"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>
static volatile sig_atomic_t stop = 0;
int main() {
    std::signal(SIGTERM, [](int) { stop = 1; });
    std::string env;
    std::ifstream e(".env");
    std::getline(e, env);
    const char *g = std::getenv("GREETING");
    pid_t child = fork();
    if (child == 0) { for (;;) pause(); }     // a child process: must die with the app
    std::printf("APP %s pid=%d child=%d env=%s greeting=%s\n", VERSION, getpid(), child, env.c_str(), g ? g : "-");
    std::fflush(stdout);
    if (std::ifstream("crash.flag")) return 3;
    while (!stop) usleep(20000);
    std::printf("APP stopping %d\n", getpid());
    std::fflush(stdout);
    return 0;
}
''')
write(".env", "MODE=one\n")
write("nodemonpp.json", '{ "env": { "GREETING": "namaste" }, "delay": 150, "ignore": ["build", "src/generated"],'
                        ' "ext": ["cc", "hpp", ".txt", "env"] }\n')

env = dict(os.environ, NO_COLOR="1")
proc = subprocess.Popen([NODEMONPP], cwd=proj, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, bufsize=1, env=env)
lines = queue.Queue()
log = []


def reader():
    for line in proc.stdout:
        lines.put(line.rstrip("\n"))
threading.Thread(target=reader, daemon=True).start()


def expect(pattern, timeout=60, forbid=None):
    """wait for a line matching pattern; returns (match, lines seen while waiting)"""
    seen = []
    end = time.time() + timeout
    while time.time() < end:
        try:
            line = lines.get(timeout=0.1)
        except queue.Empty:
            continue
        log.append(line)
        seen.append(line)
        m = re.search(pattern, line)
        if m:
            return m, seen
    print("    (timed out waiting for /%s/; last lines:)" % pattern)
    for l in log[-15:]:
        print("      |", l)
    return None, seen


def quiet(seconds):
    """collect lines for a while, expecting nothing interesting"""
    seen = []
    end = time.time() + seconds
    while time.time() < end:
        try:
            l = lines.get(timeout=0.05)
            log.append(l)
            seen.append(l)
        except queue.Empty:
            pass
    return seen


def app_line(timeout=120):
    m, seen = expect(r"APP (\S+) pid=(\d+) child=(\d+) env=(\S*) greeting=(\S+)", timeout)
    if not m:
        return None, seen
    return {"ver": m.group(1), "pid": int(m.group(2)), "child": int(m.group(3)), "env": m.group(4), "greet": m.group(5)}, seen


try:
    # 1. first start: configure + build + run, settings from nodemonpp.json
    a, seen = app_line(180)
    check(a and a["ver"] == "v1", "first start builds and runs the app (exec found from add_executable)")
    check(any("configuring" in l for l in seen) and any("built in" in l for l in seen), "first start configures + builds")
    check(a and a["greet"] == "namaste" and a["env"] == "MODE=one", "env from nodemonpp.json reaches the app")

    # 2. edit a header -> rebuild -> restart, old app + its child are gone
    time.sleep(0.3)
    write("src/version.hpp", '#define VERSION "v2"\n')
    b, seen = app_line()
    check(b and b["ver"] == "v2", "header change -> rebuilt app runs")
    check(any("src/version.hpp changed" in l for l in seen) and any("building" in l for l in seen) and
          any("restarting" in l for l in seen), "log: changed, building, restarting")
    check(any(f"APP stopping {a['pid']}" in l for l in seen) if a else False, "old app got SIGTERM and stopped cleanly")
    time.sleep(0.2)
    check(a and not alive(a["pid"]) and not alive(a["child"]), "old app and the child it started are gone")

    # 3. broken code -> build fails, the running app stays up
    write("src/version.hpp", '#define VERSION v3 +\n')
    m, seen = expect(r"build failed, old app still running", 60)
    check(m is not None, "compile error -> 'build failed, old app still running'")
    check(any("error" in l for l in seen), "compiler error is shown")
    check(b and alive(b["pid"]), "the old app is still alive after the failed build")

    # 4. fix it -> runs again
    write("src/version.hpp", '#define VERSION "v3"\n')
    c, _ = app_line()
    check(c and c["ver"] == "v3" and b and not alive(b["pid"]), "fixed code -> new app replaces the old one")

    # 5. .env -> restart without building
    time.sleep(0.3)
    write(".env", "MODE=two\n")
    d, seen = app_line()
    check(d and d["env"] == "MODE=two" and d["pid"] != c["pid"], ".env change restarts the app")
    check(not any("building" in l for l in seen) and any("without a build" in l for l in seen), ".env change skips the build")

    # 6. rs + Enter
    proc.stdin.write("rs\n")
    proc.stdin.flush()
    e, seen = app_line()
    check(e and e["pid"] != d["pid"] and any("restart requested" in l for l in seen), "`rs` rebuilds and restarts")

    # 7. ignored folder: nothing happens
    write("src/generated/auto.hpp", "// generated\n")
    seen = quiet(1.0)
    check(not any("changed" in l for l in seen) and alive(e["pid"]), "changes in an ignored folder are ignored")

    # 8. a brand-new folder inside src is watched right away
    write("src/feature/extra.hpp", "// new feature\n")
    m, seen = expect(r"src/feature/extra\.hpp changed", 10)
    check(m is not None, "a new sub-folder is picked up")
    f, _ = app_line()

    # 9. editor temp files are ignored
    write("src/.main.cc.swp", "x")
    write("src/main.cc~", "x")
    seen = quiet(0.8)
    check(not any("changed" in l for l in seen), "editor swap/backup files are ignored")

    # 10. many saves at once -> one rebuild
    for i in range(5):
        write("src/version.hpp", f'#define VERSION "v4-{i}"\n')
        time.sleep(0.02)
    g, seen = app_line()
    check(g and g["ver"] == "v4-4" and sum("building" in l for l in seen) == 1, "5 quick saves -> 1 rebuild (debounce)")

    # 11. CMakeLists.txt -> reconfigure
    with open(os.path.join(proj, "CMakeLists.txt"), "a") as fh:
        fh.write("# touched\n")
    h, seen = app_line()
    check(h and any("configuring" in l for l in seen), "CMakeLists.txt change -> cmake reconfigure")

    # 12. the app crashes -> reported, nodemonpp keeps watching
    write("crash.flag", "1")
    write(".env", "MODE=crash\n")
    m, seen = expect(r"app exited with code 3", 30)
    check(m is not None, "crash is reported with its exit code")
    os.remove(os.path.join(proj, "crash.flag"))
    write("src/version.hpp", '#define VERSION "v5"\n')
    i_, _ = app_line()
    check(i_ and i_["ver"] == "v5", "after a crash the next change starts it again")

    # 13. Ctrl+C stops nodemonpp and the app
    proc.send_signal(signal.SIGINT)
    m, seen = expect(r"bye", 15)
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        pass
    check(m is not None and proc.returncode == 0, "Ctrl+C: nodemonpp exits cleanly")
    time.sleep(0.2)
    check(i_ and not alive(i_["pid"]) and not alive(i_["child"]), "Ctrl+C: app and its child are stopped too")
finally:
    if proc.poll() is None:
        proc.kill()
    shutil.rmtree(proj, ignore_errors=True)

# command line checks
out = subprocess.run([NODEMONPP, "--help"], capture_output=True, text=True)
check(out.returncode == 0 and "Usage: nodemonpp" in out.stdout, "--help")
empty = tempfile.mkdtemp()
out = subprocess.run([NODEMONPP], cwd=empty, capture_output=True, text=True, env=env)
check(out.returncode == 2 and "no CMakeLists.txt" in out.stderr, "clear message outside a CMake project")
shutil.rmtree(empty, ignore_errors=True)

print(f"{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
