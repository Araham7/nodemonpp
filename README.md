# nodemonpp: C++ ke liye nodemon

`nodemonpp` tumhare C++ (CMake) project ki files pe nazar rakhta hai. Koi file save hote hi wo **build karta hai, purana server band karta hai aur naya chala deta hai**, bilkul Node ke `nodemon` ki tarah. Express++, Drogon ya kisi bhi CMake project ke saath chalta hai.

```
$ nodemonpp
[nodemonpp] v0.1.0  watching src CMakeLists.txt .env  (2 folders)
[nodemonpp] to restart at any time, enter `rs`
[nodemonpp] configuring...
[nodemonpp] building...
[nodemonpp] built in 3.2s
[nodemonpp] starting `./build/server`
[nodemonpp] src/main.cc changed
[nodemonpp] building...
[nodemonpp] built in 2.5s
[nodemonpp] restarting...
[nodemonpp] starting `./build/server`
```

## Install

```bash
sudo apt install cmake ninja-build libjsoncpp-dev g++ ccache    # ccache optional hai, lekin rebuild tez karta hai
cmake -S . -B build -G Ninja && cmake --build build
sudo cmake --install build      # /usr/local/bin/nodemonpp
```

Ye sirf Linux pe chalta hai, kyunki file watching ke liye Linux ka `inotify` use hota hai.

## Use

Apne project folder me (jahan `CMakeLists.txt` hai) bas itna chalao:
```bash
nodemonpp
```

Ye apne aap sab samajh leta hai:
- **Kya chalana hai:** `CMakeLists.txt` ka pehla `add_executable(server ...)` padh ke `./build/server` chalata hai.
- **Build:** `build/` folder na ho to pehle `cmake -S . -B build` chalata hai (Ninja aur ccache mile to unke saath), phir `cmake --build build`.
- **Kya dekhna hai:** `src/`, `include/`, `cmake/`, `CMakeLists.txt` aur `.env`.

## nodemon → nodemonpp

| nodemon (Node) | nodemonpp (C++) |
|---|---|
| `nodemon server.js` | `nodemonpp` (executable apne aap milta hai) |
| `nodemon --watch src` | `nodemonpp --watch src` |
| `nodemon --ignore tests` | `nodemonpp --ignore tests` |
| `nodemon -e js,json` | `nodemonpp -e cc,hpp,env` |
| `nodemon --delay 2` | `nodemonpp --delay 2000` (ms me) |
| `nodemon --exec "python app.py"` | `nodemonpp --exec "./build/server --port 4000"` |
| `nodemon.json` | `nodemonpp.json` (ya `expresspp.json` me `"nodemonpp": {...}`) |
| `rs` + Enter | `rs` + Enter |
| `nodemon server.js -- --port 4000` | `nodemonpp -- --port 4000` |

## Kya badla, to kya hota hai

| Badli hui file | Kya hota hai |
|---|---|
| `.cc` / `.hpp` / `.h` ... | build → restart. Ninja sirf badli hui files compile karta hai. |
| `CMakeLists.txt` / `*.cmake` | `cmake` dobara configure → build → restart |
| `.env` | **bina build ke** sirf restart (code nahi badla) |
| editor ki temp files (`.swp`, `~`, `.#...`) | kuch nahi |
| ignore wale folders (`build`, `.git`, ...) | kuch nahi |

Kuch aur baatein:
- **Kai files ek saath save ho:** `delay` (300 ms) tak ruk ke **ek hi baar** build karta hai.
- **Build fail ho:** compiler ka error dikhata hai aur **purana server chalta rehta hai**. Error theek karke save karo, naya server aa jayega.
- **Restart saaf hota hai:** pehle purane server ko `SIGTERM` jaata hai, taaki wo chalti requests poori kar sake. `killTimeout` (3 s) me band na ho to `SIGKILL`. Server ne jo child processes chalaye the, wo bhi band hote hain, kyunki server apne process group me chalta hai. Naya server tab hi chalta hai jab purana poori tarah band ho jaaye, isliye port free milta hai.
- **Server crash ho jaaye:** batata hai (`app exited with code 3`, `crashed (signal 11, segfault)`), aur agli file save pe dobara chalata hai.
- **Naya folder banaya:** jaise `src/feature/` banaya, to wo turant watch me aa jaata hai.
- **Ctrl+C:** server aur nodemonpp dono saaf band ho jaate hain.

## Settings

`nodemonpp.json`. Saari fields optional hain, jo nahi likhoge uska default chalega:
```json
{
  "watch": ["src", "include", "CMakeLists.txt", ".env"],
  "ignore": ["build", "src/generated"],
  "ext": [".cc", ".hpp", ".h", ".env"],
  "restartOnly": [".env"],
  "delay": 300,
  "killTimeout": 3000,
  "buildDir": "build",
  "build": "cmake --build build --target server",
  "exec": "./build/server",
  "args": ["--port", "4000"],
  "env": { "EXPRESS_ENV": "development" },
  "noBuild": false
}
```

- `env`: ye values sirf tumhare server ko milti hain. Server ko `NODEMONPP=1` bhi milta hai, jisse wo jaan sake ki nodemonpp ke andar chal raha hai.
- `ignore` me koi bhi naam (jaise `"node_modules"`) har jagah wala wo folder ignore karta hai, aur `"src/generated"` jaisa path sirf wahi folder.
- `.env` file nodemonpp khud load nahi karta, use tumhara server padhta hai (`express::env::load()`). nodemonpp bas uske badalne pe restart karta hai.
- Command line ke options file wali settings ke upar chalte hain.

## Tests

`ctest --test-dir build` ek nakli CMake app bana ke asli edits karta hai, aur **25 checks** chalata hai:
- pehla configure + build + run, `nodemonpp.json` ka env
- header badla → rebuild → restart, purana app aur uska child process band, SIGTERM se saaf stop
- compile error → purana app chalta rehta hai → fix → naya app
- `.env` → bina build ke restart
- `rs`
- ignore folder, naya sub-folder, editor temp files
- 5 jaldi-jaldi saves → 1 build
- `CMakeLists.txt` → reconfigure
- crash ka exit code → agle badlaav pe dobara start
- Ctrl+C → sab band
- `--help`, CMake project ke bahar saaf error

Ek asli Express++ app pe bhi chala ke dekha: `src/main.cc` badla, 2.5 s me rebuild hua, aur `curl` ne naya jawab diya.

## Abhi ki limits

- Sirf Linux. Mac/Windows ke liye `inotify` ki jagah doosra watcher chahiye hoga.
- Bahut bade projects me `fs.inotify.max_user_watches` badhana pad sakta hai. Zarurat padne pe nodemonpp khud bata deta hai.
- Har badlaav pe build lagta hai: ek file pe aam taur pe 1–5 s. Ise kam karne ke liye ccache install karo, aur headers kam badlo, kyunki header badalne pe usse jude saare `.cc` dobara compile hote hain.
