// nodemonpp: nodemon for C++ projects. Watches your sources, rebuilds with CMake, restarts your server.
// Linux only (inotify). See README.md.
#include <json/json.h>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static const char *VERSION = "0.1.1";

// ---------------------------------------------------------------- output
static bool gColor = false;
static bool gQuiet = false;
static bool gVerbose = false;

static std::string paint(const char *code, const std::string &s) { return gColor ? std::string("\033[") + code + "m" + s + "\033[0m" : s; }
static void say(const std::string &msg, const char *color = "33") {
    if (gQuiet) return;
    std::cerr << paint(color, "[nodemonpp] " + msg) << std::endl;
}
static void ok(const std::string &m) { say(m, "32"); }
static void bad(const std::string &m) {
    std::cerr << paint("31", "[nodemonpp] " + m) << std::endl;  // errors are shown even with --quiet
}
static void debug(const std::string &m) {
    if (gVerbose) say(m, "90");
}

static long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- config
struct Config {
    std::vector<std::string> watch{"src", "include", "CMakeLists.txt", "cmake", ".env"};
    std::vector<std::string> ignore{"build", ".git", "node_modules", "uploads", ".cache", "third_party/build"};
    std::vector<std::string> ext{".cc", ".cpp", ".cxx", ".c", ".hpp", ".hh", ".h", ".hxx", ".ipp", ".inl", ".cmake", ".txt", ".env"};
    std::vector<std::string> restartOnly{".env"};  // these restart without rebuilding (names or extensions)
    int delay = 300;               // ms to wait for more changes before acting
    int killTimeout = 3000;        // ms between SIGTERM and SIGKILL
    std::string buildDir = "build";
    std::string configure;         // "" = auto: cmake -S . -B <buildDir> [-G Ninja] [ccache]
    std::string build;             // "" = auto: cmake --build <buildDir>
    std::string exec;              // "" = auto: ./<buildDir>/<first add_executable>
    std::vector<std::string> args; // extra arguments for the app
    std::map<std::string, std::string> env;
    bool runOnStart = true;
    bool noBuild = false;          // only restart (scripts, already-built binaries)
};

static void readJsonConfig(Config &c, const Json::Value &j) {
    auto strList = [](const Json::Value &v, std::vector<std::string> &out) {
        if (!v.isArray()) return;
        out.clear();
        for (const auto &x : v) out.push_back(x.asString());
    };
    strList(j["watch"], c.watch);
    strList(j["ignore"], c.ignore);
    strList(j["ext"], c.ext);
    for (auto &e : c.ext)  // "cc" and ".cc" both work, like nodemon's "js,json"
        if (!e.empty() && e[0] != '.') e = "." + e;
    strList(j["restartOnly"], c.restartOnly);
    strList(j["args"], c.args);
    if (j.isMember("delay")) c.delay = j["delay"].asInt();
    if (j.isMember("killTimeout")) c.killTimeout = j["killTimeout"].asInt();
    if (j.isMember("buildDir")) c.buildDir = j["buildDir"].asString();
    if (j.isMember("configure")) c.configure = j["configure"].asString();
    if (j.isMember("build")) c.build = j["build"].asString();
    if (j.isMember("exec")) c.exec = j["exec"].asString();
    if (j.isMember("noBuild")) c.noBuild = j["noBuild"].asBool();
    if (j["env"].isObject())
        for (const auto &k : j["env"].getMemberNames()) c.env[k] = j["env"][k].asString();
}

static bool loadConfigFile(Config &c, const std::string &path, bool nested) {
    std::ifstream in(path);
    if (!in) return false;
    Json::CharReaderBuilder b;
    Json::Value root;
    std::string errs;
    if (!Json::parseFromStream(b, in, &root, &errs)) {
        bad(path + ": invalid JSON: " + errs);
        std::exit(2);
    }
    if (nested) {
        if (!root.isMember("nodemonpp")) return false;
        readJsonConfig(c, root["nodemonpp"]);
    } else {
        readJsonConfig(c, root);
    }
    debug("config from " + path);
    return true;
}

static void usage() {
    std::cout << R"(nodemonpp )" << VERSION << R"( - nodemon for C++ (CMake projects)

Usage: nodemonpp [options] [-- app arguments]

  -w, --watch <path>     watch this file/folder (repeatable; replaces the defaults)
  -i, --ignore <path>    ignore this folder (repeatable; added to the defaults)
  -e, --ext <list>       extensions to watch, e.g. "cc,hpp,env"
  -d, --delay <ms>       wait this long for more changes (default 300)
  -x, --exec <cmd>       command that runs the app (default ./build/<add_executable name>)
  -b, --build <cmd>      build command (default: cmake --build build)
      --no-build         never build, only restart (exec already points at something runnable)
  -q, --quiet            only errors and your app's own output
  -V, --verbose          explain what is happening
  -v, --version          print version
  -h, --help             this help

While running: type "rs" + Enter to rebuild and restart; Ctrl+C stops everything.
Settings can also live in nodemonpp.json, or under "nodemonpp" in expresspp.json.
)";
}

static std::vector<std::string> splitList(const std::string &s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) continue;
        if (item[0] != '.') item = "." + item;
        out.push_back(item);
    }
    return out;
}

// ---------------------------------------------------------------- project detection
static std::string detectExecutable(const Config &c) {
    std::ifstream in("CMakeLists.txt");
    if (!in) return "";
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    // drop comments
    text = std::regex_replace(text, std::regex("#[^\n]*"), "");
    std::smatch m;
    if (std::regex_search(text, m, std::regex(R"(add_executable\s*\(\s*([A-Za-z0-9_.+\-]+))", std::regex::icase))) {
        std::string name = m[1].str();
        if (name.find("${") == std::string::npos) return "./" + c.buildDir + "/" + name;
    }
    return "";
}

static bool have(const char *tool) {
    std::string cmd = std::string("command -v ") + tool + " > /dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

// ---------------------------------------------------------------- processes
static volatile sig_atomic_t gStop = 0;
static void onSignal(int) { gStop = 1; }

static int runShell(const std::string &cmd) {  // build / configure: wait for it, output goes to the terminal
    debug("run: " + cmd);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
        if (gStop) kill(-pid, SIGTERM);
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

struct App {
    pid_t pid = -1;
    long long startedAt = 0;
};

static std::string shellQuote(const std::string &s) {
    std::string out = "'";
    for (char ch : s) out += ch == '\'' ? std::string("'\\''") : std::string(1, ch);
    return out + "'";
}

static App startApp(const Config &c) {
    std::string cmd = c.exec;
    for (const auto &a : c.args) cmd += " " + shellQuote(a);
    pid_t pid = fork();
    if (pid < 0) {
        bad(std::string("could not start the app: ") + strerror(errno));
        return {};
    }
    if (pid == 0) {
        setpgid(0, 0);  // its own process group, so we can stop everything it starts
        for (const auto &[k, v] : c.env) setenv(k.c_str(), v.c_str(), 1);
        setenv("NODEMONPP", "1", 1);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        execl("/bin/sh", "sh", "-c", ("exec " + cmd).c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    setpgid(pid, pid);
    return {pid, nowMs()};
}

static std::string describeExit(int status) {
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 127) return "exited with code 127 (command not found? check \"exec\")";
        return "exited with code " + std::to_string(code);
    }
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        std::string name = sig == SIGSEGV ? "segfault" : sig == SIGABRT ? "abort" : sig == SIGKILL ? "killed" : sig == SIGFPE ? "floating point error" : strsignal(sig);
        return "crashed (signal " + std::to_string(sig) + ", " + name + ")";
    }
    return "stopped";
}

// stop the app's whole process group: SIGTERM, then SIGKILL after the timeout
static void stopApp(App &app, int killTimeoutMs) {
    if (app.pid <= 0) return;
    kill(-app.pid, SIGTERM);
    long long deadline = nowMs() + killTimeoutMs;
    int status = 0;
    while (true) {
        pid_t r = waitpid(app.pid, &status, WNOHANG);
        if (r == app.pid || (r < 0 && errno == ECHILD)) break;
        if (nowMs() >= deadline) {
            say("app did not stop in " + std::to_string(killTimeoutMs) + " ms, sending SIGKILL");
            kill(-app.pid, SIGKILL);
            waitpid(app.pid, &status, 0);
            break;
        }
        usleep(10000);
    }
    kill(-app.pid, SIGKILL);  // anything left in its group (child processes it started)
    app.pid = -1;
}

// ---------------------------------------------------------------- watcher
class Watcher {
  public:
    Watcher(const Config &c) : c_(c) {
        fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd_ < 0) {
            bad(std::string("inotify: ") + strerror(errno));
            std::exit(1);
        }
        root_ = fs::current_path();
    }
    int fd() const { return fd_; }

    void setup() {
        for (const auto &w : c_.watch) {
            fs::path p = root_ / w;
            std::error_code ec;
            if (fs::is_directory(p, ec))
                addTree(p);
            else {
                singleFiles_.insert(fs::weakly_canonical(p, ec).string());
                addDir(p.parent_path());  // watch the folder, filter for this file (it may not exist yet)
            }
        }
    }

    // read pending events; returns changed paths (relative to the project root)
    std::vector<std::string> read() {
        std::vector<std::string> changed;
        alignas(inotify_event) char buf[64 * 1024];
        while (true) {
            ssize_t n = ::read(fd_, buf, sizeof buf);
            if (n <= 0) break;
            for (char *p = buf; p < buf + n;) {
                auto *ev = reinterpret_cast<inotify_event *>(p);
                p += sizeof(inotify_event) + ev->len;
                auto it = dirs_.find(ev->wd);
                if (it == dirs_.end() || ev->len == 0) continue;
                fs::path full = fs::path(it->second) / ev->name;
                if ((ev->mask & IN_ISDIR) && (ev->mask & (IN_CREATE | IN_MOVED_TO))) {
                    if (insideWatchedTree(full) && !ignored(full)) {
                        addTree(full);
                        // files may have been written into it before the watch existed
                        std::error_code ec;
                        for (auto &e : fs::recursive_directory_iterator(full, ec))
                            if (e.is_regular_file() && interesting(e.path())) changed.push_back(rel(e.path()));
                    }
                    continue;
                }
                if (ev->mask & IN_ISDIR) continue;
                if (!interesting(full)) continue;
                changed.push_back(rel(full));
            }
        }
        return changed;
    }

    size_t watchCount() const { return dirs_.size(); }

  private:
    std::string rel(const fs::path &p) const {
        std::error_code ec;
        auto r = fs::relative(p, root_, ec);
        return ec ? p.string() : r.string();
    }

    bool ignored(const fs::path &p) const {
        std::string r = rel(p);
        for (const auto &ig : c_.ignore) {
            if (r == ig || r.rfind(ig + "/", 0) == 0) return true;
            // a bare name ignores that folder anywhere ("node_modules")
            if (ig.find('/') == std::string::npos) {
                for (auto &part : fs::path(r))
                    if (part == ig) return true;
            }
        }
        return false;
    }

    bool insideWatchedTree(const fs::path &p) const {
        std::error_code ec;
        std::string full = fs::weakly_canonical(p, ec).string();
        for (const auto &t : trees_)
            if (full == t || full.rfind(t + "/", 0) == 0) return true;
        return false;
    }

    bool interesting(const fs::path &p) const {
        if (ignored(p)) return false;
        std::error_code ec;
        std::string full = fs::weakly_canonical(p, ec).string();
        if (singleFiles_.count(full)) return true;
        if (!insideWatchedTree(p)) return false;
        std::string name = p.filename().string();
        if (name.empty() || name.back() == '~' || name.rfind(".#", 0) == 0 || name.find(".swp") != std::string::npos ||
            name.find(".swx") != std::string::npos || name == "4913")
            return false;  // editor temp files
        std::string ext = p.extension().string();
        for (const auto &e : c_.ext)
            if (e == ext || e == name) return true;
        return false;
    }

    void addDir(const fs::path &d) {
        std::error_code ec;
        std::string path = fs::weakly_canonical(d, ec).string();
        if (watched_.count(path)) return;
        int wd = inotify_add_watch(fd_, path.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE | IN_DELETE | IN_ATTRIB);
        if (wd < 0) {
            if (errno == ENOSPC)
                bad("too many folders to watch: raise fs.inotify.max_user_watches or add folders to \"ignore\"");
            return;
        }
        dirs_[wd] = path;
        watched_.insert(path);
    }

    void addTree(const fs::path &d) {
        std::error_code ec;
        std::string path = fs::weakly_canonical(d, ec).string();
        trees_.insert(path);
        if (ignored(d)) return;
        addDir(d);
        for (auto it = fs::recursive_directory_iterator(d, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_directory(ec)) {
                if (ignored(it->path())) {
                    it.disable_recursion_pending();
                    continue;
                }
                addDir(it->path());
            }
        }
    }

    const Config &c_;
    int fd_;
    fs::path root_;
    std::map<int, std::string> dirs_;
    std::set<std::string> watched_, trees_, singleFiles_;
};

// ---------------------------------------------------------------- main
int main(int argc, char **argv) {
    gColor = isatty(STDERR_FILENO) && !std::getenv("NO_COLOR");
    if (std::getenv("FORCE_COLOR")) gColor = true;

    Config c;
    bool cliWatch = false;
    std::vector<std::string> cliWatchList, cliIgnore, cliExt, appArgs;
    std::string cliExec, cliBuild;
    int cliDelay = -1;
    bool cliNoBuild = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                bad("missing value after " + a);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--") {
            for (i++; i < argc; i++) appArgs.push_back(argv[i]);
            break;
        } else if (a == "-w" || a == "--watch") {
            cliWatch = true;
            cliWatchList.push_back(next());
        } else if (a == "-i" || a == "--ignore") cliIgnore.push_back(next());
        else if (a == "-e" || a == "--ext") cliExt = splitList(next());
        else if (a == "-d" || a == "--delay") cliDelay = std::atoi(next().c_str());
        else if (a == "-x" || a == "--exec") cliExec = next();
        else if (a == "-b" || a == "--build") cliBuild = next();
        else if (a == "--no-build") cliNoBuild = true;
        else if (a == "-q" || a == "--quiet") gQuiet = true;
        else if (a == "-V" || a == "--verbose") gVerbose = true;
        else if (a == "-v" || a == "--version") {
            std::cout << VERSION << "\n";
            return 0;
        } else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else {
            bad("unknown option " + a + " (see nodemonpp --help)");
            return 2;
        }
    }

    // config files, then command line on top
    if (!loadConfigFile(c, "nodemonpp.json", false)) loadConfigFile(c, "expresspp.json", true);
    if (cliWatch) c.watch = cliWatchList;
    for (auto &x : cliIgnore) c.ignore.push_back(x);
    if (!cliExt.empty()) c.ext = cliExt;
    if (cliDelay >= 0) c.delay = cliDelay;
    if (!cliExec.empty()) c.exec = cliExec;
    if (!cliBuild.empty()) c.build = cliBuild;
    if (cliNoBuild) c.noBuild = true;
    if (!appArgs.empty()) c.args = appArgs;

    bool cmakeProject = fs::exists("CMakeLists.txt");
    if (!c.noBuild && !cmakeProject && c.build.empty()) {
        bad("no CMakeLists.txt here. Run nodemonpp in your project folder, or pass --build / --no-build");
        return 2;
    }
    std::string ninja = have("ninja") ? " -G Ninja" : "";
    std::string ccache = have("ccache") ? " -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache" : "";
    if (c.configure.empty() && cmakeProject)
        c.configure = "cmake -S . -B " + shellQuote(c.buildDir) + (fs::exists(c.buildDir + "/CMakeCache.txt") ? "" : ninja + ccache) +
                      " -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON";
    if (c.build.empty()) c.build = "cmake --build " + shellQuote(c.buildDir);
    if (c.exec.empty()) c.exec = detectExecutable(c);
    if (c.exec.empty()) {
        bad("could not find add_executable(...) in CMakeLists.txt. Tell me what to run: --exec ./build/myserver");
        return 2;
    }

    struct sigaction sa {};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    Watcher w(c);
    w.setup();
    std::string watching;
    for (const auto &x : c.watch)
        if (fs::exists(x)) watching += (watching.empty() ? "" : " ") + x;
    say(std::string("v") + VERSION + "  watching " + (watching.empty() ? "(nothing yet)" : watching) + "  (" +
        std::to_string(w.watchCount()) + " folders)");
    say("to restart at any time, enter `rs`");
    debug("exec: " + c.exec);
    debug("build: " + (c.noBuild ? std::string("(none)") : c.build));

    App app;
    bool needConfigure = cmakeProject && !c.noBuild && !fs::exists(c.buildDir + "/CMakeCache.txt");
    bool lastBuildFailed = false;

    // build (if needed) and (re)start; keeps the old app running when the build fails
    auto cycle = [&](bool rebuild, bool reconfigure, const std::string &why) {
        if (!why.empty()) say(why);
        if (rebuild && !c.noBuild) {
            if (reconfigure || needConfigure) {
                say("configuring...");
                if (runShell(c.configure) != 0) {
                    bad("cmake configure failed" + std::string(app.pid > 0 ? ", old app still running" : "") + " - fix it and save again");
                    lastBuildFailed = true;
                    return;
                }
                needConfigure = false;
            }
            say("building...");
            long long t0 = nowMs();
            int rc = runShell(c.build);
            if (gStop) return;
            if (rc != 0) {
                bad("build failed" + std::string(app.pid > 0 ? ", old app still running" : "") + " - fix it and save again");
                lastBuildFailed = true;
                return;
            }
            char secs[32];
            std::snprintf(secs, sizeof secs, "%.1f", (nowMs() - t0) / 1000.0);
            ok(std::string("built in ") + secs + "s");
        }
        lastBuildFailed = false;
        if (app.pid > 0) {
            say("restarting...");
            stopApp(app, c.killTimeout);
        }
        say("starting `" + c.exec + "`", "32");
        app = startApp(c);
    };

    if (c.runOnStart) cycle(true, false, "");

    std::set<std::string> pending;
    long long pendingSince = 0;
    std::string stdinBuf;
    bool stdinOpen = true;

    while (!gStop) {
        pollfd fds[2] = {{w.fd(), POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
        int nfds = stdinOpen ? 2 : 1;
        int timeout = pending.empty() ? 200 : 20;
        int r = poll(fds, nfds, timeout);
        if (r < 0 && errno != EINTR) break;

        if (r > 0 && (fds[0].revents & POLLIN))
            for (auto &p : w.read()) {
                if (pending.empty()) pendingSince = nowMs();
                pending.insert(p);
                debug("changed: " + p);
            }

        if (r > 0 && stdinOpen && (fds[1].revents & (POLLIN | POLLHUP))) {
            char b[256];
            ssize_t n = ::read(STDIN_FILENO, b, sizeof b);
            if (n <= 0)
                stdinOpen = false;  // no terminal (CI, background): keep watching
            else {
                stdinBuf.append(b, static_cast<size_t>(n));
                size_t nl;
                while ((nl = stdinBuf.find('\n')) != std::string::npos) {
                    std::string line = stdinBuf.substr(0, nl);
                    stdinBuf.erase(0, nl + 1);
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                    if (line == "rs") {
                        pending.clear();
                        cycle(true, false, "restart requested");
                    }
                }
            }
        }

        // the app ended on its own?
        if (app.pid > 0) {
            int status = 0;
            pid_t done = waitpid(app.pid, &status, WNOHANG);
            if (done == app.pid) {
                kill(-app.pid, SIGKILL);
                std::string what = describeExit(status);
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                    say("app " + what + " - waiting for changes before restart");
                else
                    bad("app " + what + " - waiting for file changes before restart");
                app.pid = -1;
            }
        }

        if (!pending.empty() && nowMs() - pendingSince >= c.delay) {
            bool reconfigure = false, rebuild = false;
            std::string first;
            for (const auto &p : pending) {
                std::string name = fs::path(p).filename().string(), ext = fs::path(p).extension().string();
                bool restartOnly = std::find(c.restartOnly.begin(), c.restartOnly.end(), name) != c.restartOnly.end() ||
                                   std::find(c.restartOnly.begin(), c.restartOnly.end(), ext) != c.restartOnly.end();
                if (name == "CMakeLists.txt" || ext == ".cmake") reconfigure = true;
                if (!restartOnly) rebuild = true;
                if (first.empty()) first = p;
            }
            std::string why = first + " changed" + (pending.size() > 1 ? " (+" + std::to_string(pending.size() - 1) + " more)" : "");
            pending.clear();
            if (!rebuild && lastBuildFailed) {
                say(why + " - last build failed, rebuilding first");
                rebuild = true;
            } else if (!rebuild)
                why += ", restarting without a build";
            cycle(rebuild || reconfigure, reconfigure, why);
        }
    }

    if (app.pid > 0) {
        say("stopping app...");
        stopApp(app, c.killTimeout);
    }
    say("bye");
    return 0;
}
