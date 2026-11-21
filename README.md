# CookieBot

A Windows bot that **plays games by looking at the screen**, like a person. It has two modes:

| Mode | What it does |
|---|---|
| **Cookie mode** (default) | Auto-plays *COOKIES FOR U* (cookiesforu.netlify.app/game): clicks the cookie at 12/s, plans and buys upgrades, clicks bonus cookies, closes popups, smashes the "Are you a real baker?" puzzle, refreshes the page if it breaks. |
| **Copycat** (F6 / F7) | Works on **any** windowed game. Watches you play, then plays by copying what you did in similar-looking moments. It reads numbers on screen (health, food, wood…) to learn what's good or bad, runs a real object detector (YOLOX), learns your hotbar, and follows rules and tasks you teach it in plain English. |

It updates itself from this repo (`binjin-maker/cookie-cwicker`).

---

## Contents
1. [Install & run](#1-install--run)
2. [Controls](#2-controls)
3. [Cookie mode](#3-cookie-mode)
4. [Copycat mode](#4-copycat-mode)
5. [F10: talking to Copycat](#5-f10-talking-to-copycat)
6. [Screen overlays](#6-screen-overlays)
7. [Files it creates](#7-files-it-creates)
8. [Source code map](#8-source-code-map)
9. [Building the exe](#9-building-the-exe)
10. [Releasing an update (push)](#10-releasing-an-update-push)
11. [Changing things (tunables)](#11-changing-things-tunables)
12. [Fixing things (troubleshooting)](#12-fixing-things-troubleshooting)
13. [Safety rules & limits](#13-safety-rules--limits)

---

## 1. Install & run

1. Download `CookieBot.ex_` from this repo and rename it to **`CookieBot.exe`** (it's stored as `.ex_` so browsers don't block it).
2. Put it in its own folder (it writes logs and data next to itself) and run it.
3. First run: it downloads ~95 MB of object-detector files next to itself (`onnxruntime.dll`, `DirectML.dll`, `onnxruntime_providers_shared.dll`, `yolox_tiny.onnx`, `yolox_s.onnx`). The log shows progress. Restart once after they finish.
4. Needs **Windows 10/11** with an OCR language installed (English normally is).

Command-line options (optional): `--hours N` session length (default 8) · `--cps N` clicks/s, max 12 · `--nobuy` · `--nogolden` · `--nofx` (no overlays).

**Updating:** it checks GitHub at start, every hour, and when you press **Check for updates** (or F10 `update`). New versions download, it saves, swaps itself and restarts.

---

## 2. Controls

| Key | Action |
|---|---|
| **F6** | Copycat: start/stop **watching** you play (click the game first) |
| **F7** | Copycat: start/stop **letting it play** |
| **F8** | Cookie mode: pause / resume (3 s hands-off countdown) |
| **F9** | Quit |
| **F10** | Copycat: open the "Tell me something" prompt (it pauses while you type; Enter sends, Esc cancels) |
| Any letter (cookie mode) | Opens the cookie **order** prompt in the HUD |
| Touch mouse | Cookie mode: pauses. Copycat playing: you take over (it records you), it resumes 2 s after you let go |

**Control panel window:** status, Pause/Resume, Quit, toggles (Buy upgrades / Bonus cookies / Effects+HUD), session hours + clicks/s, stats, live log, *Forget what it learned*, *Check for updates*, *Watch me play (F6)*, *Let it play (F7)* (the panel buttons give you 3 s to click into your game).

---

## 3. Cookie mode

1. Run CookieBot, click the browser tab with the game (title must contain "cookies"). It counts down 3 s, then plays.
2. Keep the store's buy amount on **x1** and don't change browser zoom while it runs.

**How it plays (main loop in `main()`):**
- Finds the layout by reading the screen (the *Store* and *Your bakery* headings, only text inside the game window).
- Finds the big cookie by colour, or by shape if the cookie flavour changed.
- Reads the real cookie count **every second** on its own thread, and keeps the last value.
- Scans the whole store **once an hour** (prices only change when you buy). During a scan it hovers every building to read its exact "each X makes N cookies per second" tooltip.
- After every buy it re-reads just that store page and updates prices. After an upgrade buy it re-reads the bakery panel.
- **Planner** (`plan()` in `logic.h`): simulates buying strategies over 4 h (plus a 1 h tail) and picks the purchase that ends with the most cookies. It counts the clicks it actually achieves (not a perfect 12/s).
- Clicks bonus cookies, closes "Welcome back"/"New cookie" popups.
- **Baker puzzle** ("Are you a real baker?"): a big red deadeye appears and it clicks the screen centre up to 530 times, re-checking every ~1 s and stopping when the puzzle is gone. Real captchas: it beeps and waits for you.
- Only refreshes (F5) if the page is actually blank or an error page for 20+ s.

**Orders** (type any letter while it's running): `buy 10 grandmas`, `save up for a dough portal`, `skip bakery`, `allow bakery`, `stop buying`, `bonus off`, `clicks 8`, `6 hour session`, `rescan`, `update`, `pause`, `help`. Orders survive until done and are queued if the store hasn't been read yet.

---

## 4. Copycat mode

### Basic loop
1. Click your game (borderless/windowed: true fullscreen often captures black).
2. **F6** → play normally → **F6** to stop. It records 30 frames/s.
3. **F7** → it plays. **F7** again to stop.
4. Grab the mouse/keys any time to correct it. Corrections count **double**.

### What it sees ("eyes")
- A **64×36** grey view of the game + 8 colour tiles + a sharp **24×24 crosshair close-up** grabbed from the full-resolution screen.
- Every **number on screen** with the label next to it (~8×/s).
- **Detections** from YOLOX (80 everyday classes: person, car, truck, boat, airplane, dog, horse, cow, bear, bird, bottle, knife, chair…) with screen coordinates and, once the camera is calibrated, mouse-count offsets from the crosshair.
- **Moving things** (motion left after removing the camera's own movement), with colour and shape.
- **Hotbar slot** you're holding (learned when you press 1-9).
- **What's under the crosshair** (learned when hitting it raised a stat, or when you named it).

### How it decides
- Finds remembered moments that look like now (fast 16×9 thumbnail pass over all memories, then a sharp comparison on the best 400) and **follows one** for up to ~0.8 s, then re-checks. Following beats averaging (no jitter).
- Prefers moments that turned out well, moments where you were actually doing something, moments where the enemy was in the same part of the screen as now, and moments that raised a stat it needs (e.g. low food).
- **Learns while playing alone:** good/bad results credit the last 5 s of its own actions **and** the memories it was copying. If nothing happens for 25 s it gets bored and tries its 2nd/3rd best idea. Walking into a wall for 3 s counts as bad.

### Good and bad
- Numbers going up/down: health/armor/food/water/stamina (up = good), score/kills/coins/wood/stone/metal/scrap/loot (up = good), radiation/cold/poison (up = bad).
- Ignored: ammo, time, fps, ping, coordinates, wet, temperature, comfort, weight, any number that ticks constantly, and any number that keeps rising **while nothing is pressed** (auto-detected).
- A red screen flash = damage.
- Things detected right before bad events become "probably enemies". **What you tell it always wins** over what it worked out.
- Your own gun/arms (things that sit at the bottom of the screen most of the time) are marked *yours* and never count.

### Camera skills
- Learns how far the view turns per mouse count by watching you (HUD **CAMERA** row).
- Looks around when idle/bored, turns away when it's walking into something, turns to look at animals/items it considers good.
- Takes camera commands from F10 (see below).
- Can aim at people and enemies too: `look at the person`, `aim at` steps and "when you see a <thing>" tasks all work on them.

---

## 5. F10: talking to Copycat

Press **F10**, type, **Enter**. Everything is saved per game in `copycat\<game>.rules` / `.tasks`.

### Rules about the world
| You type | Effect |
|---|---|
| `person is enemy` / `bears are dangerous` / `cows are food` | Sets what a detected class is (permanent, beats learning) |
| `airplane is my gun` / `that's my gun` | Marks a detection as **yours** (grey box, never good/bad), renames it |
| `bottles are barrels` | Just renames a class |
| `reset names` | Undo all renames |
| `red ones are enemies` / `green ones are friendly` | Colours of moving things |
| `wet doesn't matter` / `ignore wet` | Ignore that number |
| `radiation is bad` / `scrap is good` | Which direction is good |
| `this is the base` | Names the current "place" |
| `this is dangerous` / `this is good` | Marks this moment |
| `that's a tree` / `looking at a crate` | Remembers what's under the crosshair |
| `3 is the hatchet` | Names hotbar slot 3 |
| `don't press e` / `never click` / `you can press e` | Key bans |
| `ask me` / `don't ask` | Whether it may ask "What are you doing?" when lost (off by default) |

### Camera commands (while it plays)
`look at 800 400` (screen pixels; the HUD **WHERE** row shows positions) · `turn left 90` · `turn right 30` · `turn around` · `look up 20` · `look at the bear` / `look at the person`.

### Teaching by showing (recorded tasks)
```
teach heal when health is low      <- then do it yourself
done                               <- F10 again when finished
```
It replays exactly what you pressed whenever the condition is true (and at most every 10 s). If the condition is "when you see a <thing>", it turns to face the thing first.

### Teaching by explaining (step scripts)
```
teach hunt when you see a cow: look at it then walk until close then hit 3 times
if detect barrel then aim at barrel offset -40 20 then hold rmb && lmb for 300ms
when you see a dog, look at it then walk to it then press e
when low on health run away from person
teach loot: press e then wait 1 then press tab        (run it with: do loot)
```

**Conditions:** `when <number> is low / below 30 / empty / high / above 80 / full` · `hungry/thirsty/hurt` · `when you see / detect a <thing>` · `when an enemy is ahead` · `when looking at <thing>` · `every 2 minutes / every 30 seconds` · none = only when you say `do <name>`.

**Steps** (split by `then` or commas):

| Step | Meaning |
|---|---|
| `look at / aim at <thing> [offset X Y]` | Turn until it's under the crosshair; offset = pixels from its centre (X right, Y down) |
| `turn left/right/up/down N`, `turn around` | Turn by degrees (assumes ~90° view) |
| `walk/run forward/back/left/right [N s]` | Move |
| `walk until close` / `walk to it` | Walk, steering at the thing, until it's big on screen |
| `run away` / `flee` / `retreat` | Turn around + sprint |
| `click`, `click lmb`, `click rmb`, `right click`, `click x3`, `hit for 2 seconds` | Clicks |
| `hold lmb for 500ms`, `hold rmb && lmb for 300ms`, `hold w and shift for 2s` | Hold together |
| `press e`, `press tab 2 times`, `use`/`loot`/`eat` (E), `jump`, `reload`, `crouch N` | Keys |
| `wait N` | Pause |
| `do <other task>` | Chain tasks |

`it`/`them` = the thing from the condition. Times accept `ms`, `s`, `sec`, `seconds`.

---

## 6. Screen overlays
All overlays are excluded from CookieBot's own screenshots. Toggle with **Effects + HUD**.

- **HUD (lower left):** cookie mode shows state, task, what it wants, why, alternatives, timers, stats, thoughts and the order prompt. Copycat shows mode, speed, SEES, WHERE (coordinates), CAMERA, MOVING, ANALYZING, TRYING TO GET, DOING, CONFUSED, WRITING DOWN and the F10 prompt. **Drag it:** touch the mouse (pauses), drag, F8. The position is saved in `cookiebot_hud.txt`.
- **Brain view (lower right):** live neurons. SEES → MEMORIES/OPTIONS → WANTS → DOES. Every neuron is a real signal.
- **Detection boxes:** red = enemy, green = friendly, yellow = unknown, grey = yours.
- **Green reading box** with a scan line: where it's reading text right now.
- **Dotted path:** where the cursor is going (cookie mode).
- **Blue vignette, cursor ring, click bursts, red deadeye on the baker puzzle.**

---

## 7. Files it creates
All next to the exe.

| File | What |
|---|---|
| `cookiebot_log.txt` | Everything it decided/did (appends) |
| `cookiebot_debug.txt` | Raw OCR text for failed reads, store pages, buys (wiped each start) |
| `cookiebot_crash.txt` | Written on any crash: version, code address and what each thread was doing |
| `cookiebot_brain.txt` | Cookie-mode learning (delete or *Forget* to reset) |
| `cookiebot_hud.txt` | Where you dragged the HUD |
| `copycat\<game>.rec` | Copycat recording (frames, places, answers). Pre-v22 files are kept as `.rec.old` |
| `copycat\<game>.rules` | Bans, names, classes, stat meanings, camera calibration, mover colours |
| `copycat\<game>.tasks` | Taught tasks and scripts |
| `copycat\<game>.tools` / `.looks` | Hotbar slot pictures / crosshair things it recognises |
| `copycat\<game>_journal.txt` | Readable notes: what it saw, learned, got confused by, and why it was idle |
| `CookieBot.exe.old` / `.new` | Leftovers from updates (deleted on start) |

`<game>` is the game's exe name (e.g. `Ferro`). To reset Copycat for a game, delete its files in `copycat\`.

---

## 8. Source code map

```
src/
  main.cpp        Windows app: cookie mode, overlays, input, OCR, updater, panel, main()
  logic.h         Portable C++ (testable on Linux): parsing, layout, economy, planner, orders
  copycat.h       Copycat: capture, memory search, rewards, places, tools, rules, tasks, camera, HUD
  detect.h        Object detector: ONNX Runtime C API (loaded at runtime), YOLOX decode, tracking, box overlay
  brain.h         Brain (neuron) view
  cookiebot.rc    Resource script (icon)
  cookiebot.ico   Icon (made by icon_make.py)
  icon_make.py    Regenerates the icon with Pillow
```

**main.cpp, in order:** config & globals (`VERSION`, `REPO_RAW`) · prompt/order globals · HudState/logging · overlay thread (vignette, cursor FX, dotted path, reading box, scan line, deadeye) · `glideTo`/`safeClick` · LL mouse/keyboard hooks (`mouseHook`, `keyHook`) + `inputThread` (F6-F10) · `grab()` screenshots · WinRT OCR (`ocrInit`, `ocrImage`, `ocrRect`) · cookie vision (`locateMainCookie`, `spawnedCookies`) · `readEconNow` + `econThread` · `detectLayout`, `connectionWatch` · `screenGuard` (popups/puzzle) · `readTip`, `fullScan` · `buy` · `renderHud` · `#include "detect.h"`, `"copycat.h"`, `"brain.h"` · panel (`drawButton`, `panelProc`, `panelThread`) · `clickFor` · orders (`processCmds`, `checkGoal`) · updater (`httpGet`, `fetchExtras`, `checkUpdate`, `updateThread`, `applyUpdate`) · crash handler · `main()`.

**copycat.h key functions:** `capture` (eyes) · `dist`/`distThumb` (SSE comparison) · `findMovers` · `placeFor` · `reward` (credit/blame) · `ocrThread` (numbers) · `detThread` (detector + overlay) · `updateSeen`/`stampEnemy` (coordinates) · `camTurnPixels`/`camStep` (camera) · `analyze` (findings) · `explain` (F10 rules) · `parseScript`/`parseWhen`/`taskCommand`/`taskDue` (tasks) · `decide` (memory search) · `loop` (30 fps main loop) · `start`/`stop` · `renderHud`.

---

## 9. Building the exe

### On Windows (MSYS2, MinGW-w64)
```bat
pacman -S mingw-w64-x86_64-gcc
cd src
windres cookiebot.rc -O coff -o cookiebot.res
g++ -O2 -s -std=c++17 -static -mwindows -o CookieBot.exe main.cpp cookiebot.res ^
    -lruntimeobject -lwinmm -lole32 -luser32 -lgdi32 -lwininet
```

### Cross-compiling from Linux
```bash
sudo apt install g++-mingw-w64-x86-64-posix
cd src
x86_64-w64-mingw32-windres cookiebot.rc -O coff -o cookiebot.res
x86_64-w64-mingw32-g++-posix -O2 -s -std=c++17 -static -mwindows \
    -o CookieBot.exe main.cpp cookiebot.res \
    -lruntimeobject -lwinmm -lole32 -luser32 -lgdi32 -lwininet
```

`build.bat` and `build.sh` in this repo do exactly that.

- Drop `-mwindows` to get a console window (handy for `printf` debugging).
- Drop `-s` to keep symbols. Do that when chasing a crash (see §12).
- `logic.h` compiles with any g++, so you can unit-test parsers/planner on Linux/Mac.
- The detector needs nothing at build time: `onnxruntime.dll` is loaded with `LoadLibrary` at runtime.

---

## 10. Releasing an update (push)

1. Make your change in `src/`.
2. Bump the version in `src/main.cpp`: `static const int VERSION = 38;` → `39`.
3. Build (§9) and rename the result to **`CookieBot.ex_`** in the repo root.
4. Put the same number in **`version.txt`** (just `39`).
5. Commit and push to `main`:
   ```bash
   git add -A
   git commit -m "v39: what changed"
   git push origin main
   ```
Running copies see the new version within seconds of a check (they read files pinned to the newest commit, so there's no GitHub cache delay), download it, and restart into it.

**Extra files** (DLLs, models) are listed in `extras.txt` as `<file in repo> <name on disk> <size in bytes>`. Store DLLs as `.dl_`. CookieBot downloads any that are missing or the wrong size. To ship a new model: add the file, add a line with its exact size (`stat -c%s file`), push.

> GitHub's per-file limit is 100 MB. Keep models under that.

---

## 11. Changing things (tunables)
Search for the quoted text in the file named.

### Cookie mode (`main.cpp` unless noted)
| What | Where |
|---|---|
| Default clicks/s, session hours | `static double cfgCps = 12.0, cfgHours = 8.0;` |
| Gap between clicks (12/s cap) | `safeClick()`: `1.0 / std::min(12.0, cfgCps) + 0.0004` |
| Click spread on cookie | `clickFor()`: `rand() % 11 - 5` (±5 px) |
| Glide speed/curve | `glideTo()`: `d / 3800.0 + 0.06`, `amp = d * 0.08` |
| Full store rescan | `now() - gLastFull > 3600` (also HUD `gLastFull + 3600`) |
| Cookie count read rate | `econThread()`: `now() - gEconTime < 1.0` |
| Tooltip wait | `readTip()`: `nap(280)` |
| Refresh rules | `connectionWatch()`: `now() - gFailSince < 20`, `blank = n < 6` |
| Skip time after a failed buy | `buy()`: `gBlack[cur.key] = now() + (… ? 20 : 180)` |
| Planner look-ahead | `logic.h plan()`: `std::min(sessionLeft, 4 * 3600.0), tail = 3600` |
| Upgrade wording | `logic.h understand()` |
| Layout anchors | `logic.h findLayout()` ("store", "your bakery"…) |
| Never-click words | `updateDanger()` `bad[]` |
| Puzzle click count / re-check | `screenGuard()`: `530`, `done % 12 == 11` |
| Popup buttons | `dismissPopup()` `ok[]` |
| Order wording | `logic.h parseOrder()` |
| HUD size/position | `renderHud()`: `470 * k, 412 * k`, last lines |

### Copycat (`copycat.h`)
| What | Where |
|---|---|
| Eye size | top: `FW = 64, FH = 36`, `CPW = 24` (changing these breaks old recordings) |
| Memory size | `MAXFRAMES = 40000` (~22 min) |
| Neighbours / first-pass size | `K = 12`, `rough.size() < 400` |
| How long it follows one memory | `clipLeft = 24` (frames), re-pick if `dRef * 2.6` |
| Asking when lost | `confusion > 2.2`, `now() - lastAsk > 180`, `askAllowed` |
| Credit window | `reward()`: `t - data[i].t < 5.0`, decay `/ 2.5` |
| Idle memories weight | `w *= 0.25` |
| Corrections weight | `if (fr.self == 2) w *= 2;` |
| Boredom | `now() - lastRewardAt > 25` |
| Stuck penalty | `now() - stuckSince > 3` |
| Key rate limit | `applyOut()`: `t - keyChanged[vk] < 0.16` |
| Never-pressed keys | `neverPress()` |
| Mouse cap per frame | `std::max(-80, std::min(80` |
| Camera turn speed | `camStep()`: `camX * 0.25`, `±60` |
| "Close enough" for walk-until-close | `hf > 0.35` |
| Number meanings | `weightFor()` |
| Your-gun detection | `markOwn()`: `0.985f`, `p > 0.5f`, `H * 0.85` |
| Ignore swaying gun motion | `movePres[cell] > 0.6f` |
| Number-reading rate | `ocrThread()`: `Sleep(120)` |
| Save interval | `now() - lastSave > 600` |

### Detector (`detect.h`)
| What | Where |
|---|---|
| Model | `load()`: `yolox_tiny.onnx` (416) or `yolox_s.onnx` (640) |
| Confidence | `conf < 0.45f` |
| Box smoothing / persistence | `track()`: `a = 0.45f`, `> 0.4` s, `hits >= 3` |
| Classes | `COCO[80]` |

**Adding a new F10 phrase:** world rules go in `explain()`, task/camera commands in `taskCommand()`, script steps in `parseScript()`, conditions in `parseWhen()`/`taskDue()`.

---

## 12. Fixing things (troubleshooting)

**Where to look first:** `cookiebot_log.txt` → `copycat\<game>_journal.txt` → `cookiebot_debug.txt` → `cookiebot_crash.txt`.

| Symptom | Likely cause / fix |
|---|---|
| "couldn't read cookie count" | `cookiebot_debug.txt` "econ read FAILED" shows the raw OCR. Fix matching in `readEcon()` (`logic.h`) |
| "can't find the game layout" | The log line lists how many store/bakery headings it saw *in the game window*. Check `findLayout()` anchors / zoom |
| Keeps refreshing | It only refreshes if the page body has < 6 words or the title is an error page; see `connectionWatch()` |
| Lost the cookie | New flavour? It falls back to shape. See `locateMainCookie()` |
| Clicks but never buys | Debug file: any `ITEM` lines under "store @N notches"? If not, `parseStore()` doesn't recognise the cards |
| Copycat does nothing | Journal says why ("no memory to follow" = record more with F6; "matches are you standing still") |
| Copycat stopped by itself | Journal: "game isn't in front" / "mouse left the game window" (safety) |
| Detector not running | HUD SEES row shows status; files missing → wait for download/restart |
| Effects visible in screenshots | Old Windows without capture exclusion → run with `--nofx` |
| It crashes | Send/read `cookiebot_crash.txt`: `exe+0xADDR` + what each thread was doing. Rebuild **without `-s`**, then `x86_64-w64-mingw32-addr2line -e CookieBot.exe 0xADDR` (add the image base 0x140000000 if needed) |
| It freezes | Usually two threads locking in opposite order. Rule: lock `cc::mu` before `cc::amu` before `cc::bmu` before `det::mu`, never the reverse |
| Silent close, no crash file | Stack overflow or Windows killing it (check Windows Security → Protection history) |
| Weird decisions after a game update | *Forget what it learned* (cookie) or delete the game's `copycat\` files |

---

## 13. Safety rules & limits

**Cookie mode** never clicks outside the game panels or on text like reset, shop, cart, sell, delete, log out, wipe, ascend, checkout, claim (`updateDanger()`). The only key it presses is F5. Every click re-checks: not paused, game window in front, safe point.

**Copycat:**
- Only acts while the game it started on is the foreground window, and stops completely if that changes or a visible mouse pointer leaves the game window.
- Never presses Windows/Alt/Esc/F-keys/Delete/media keys; each key/button changes at most ~6×/s; mouse moves are capped per frame.
- Any physical mouse/keyboard input hands control back to you instantly.

**Limits to expect:** Copycat is mostly "find the closest memory and copy it" plus rules. Its main view is small, the detector only knows 80 everyday classes, and it learns from minutes of your play. It's a funny, slowly-improving copycat, not a real game AI. More recording plus more corrections = better play.

---

Third-party: ONNX Runtime (MIT), DirectML (Microsoft redistributable), YOLOX (Apache-2.0). See `THIRD_PARTY.txt`.
