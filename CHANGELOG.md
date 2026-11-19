# Changelog

## [0.1.1] - 2026-11-19

- **Fix:** config file (`nodemonpp.json` / `expresspp.json`) me `"ext": ["cc", "hpp"]` bina dot ke bhi kaam karta hai (pehle sirf `".cc"` chalta tha, aur bina dot wali file changes ignore ho jaati thi). Node ke nodemon jaisa.

## [0.1.0] - 2026-11-19

Pehli release.

- inotify se files dekhna (recursive, naye folders bhi), ignore list, editor temp files filter, debounce
- Badlaav ke hisaab se kaam: code → build, CMake files → reconfigure + build, `.env` → bina build ke restart
- CMake ka apne aap pata lagana: `add_executable` se server, configure (Ninja/ccache), `cmake --build`
- Build fail ho to purana app chalta rehta hai; restart me process group ko SIGTERM, phir timeout pe SIGKILL
- `rs` command, Ctrl+C se saaf band, crash/exit code report
- `nodemonpp.json` / `expresspp.json` config, CLI options, app ke liye env aur args
- 25 end-to-end tests (Python driver + asli CMake app); asli Express++ app pe bhi check kiya
