====================================================================
 COOKIEBOT  -  what it does and how to change it
 Auto-player for COOKIES FOR U (cookiesforu.netlify.app/game)
====================================================================

CONTENTS
  1.  Quick start
  2.  Controls
  3.  The control panel
  4.  The HUD (lower-left readout)
  5.  How it plays, step by step
  6.  Reading the screen (OCR)
  7.  The brain: what it learns and where it's stored
  8.  The planner: how it decides what to buy
  9.  Bonus cookies, popups, captchas, connection loss
  10. Safety rules (what it will never click)
  11. Files it creates
  12. Source code map
  13. How to change things (tunables, with exact locations)
  14. How to rebuild the exe
  15. Troubleshooting
  16. Auto-update
  17. Prompt bar (typed orders)
  18. Copycat (watch you play any game, then play it)


--------------------------------------------------------------------
1. QUICK START
--------------------------------------------------------------------
  - Rename CookieBot.ex_ to CookieBot.exe and run it.
  - The control panel opens. Click the browser tab with the game.
  - The bot finds the tab (its title has "cookies" in it), counts
    down 3 seconds, then starts. Let go of the mouse during the
    countdown.
  - Keep the store's buy amount set to x1. The bot also clicks x1
    once at startup.
  - Keep the browser zoom the same while it runs. If you change it,
    pause and resume so it re-finds the layout.

  Needs Windows 10 or 11 with an OCR language installed (English is
  normally already there). Command-line options:
      --hours N     session length in hours (default 8)
      --cps N       clicks per second, max 12 (default 12)
      --nobuy       only click, never buy
      --nogolden    don't click bonus cookies
      --nofx        no vignette, cursor effects or HUD


--------------------------------------------------------------------
2. CONTROLS
--------------------------------------------------------------------
  F8            pause / resume (resume gives a 3 s hands-off countdown)
  F9            quit
  Touch mouse   instantly pauses. Physical mouse movement is detected
                by a low-level hook; the bot's own moves are flagged
                "injected" and ignored.
  Panel         Pause/Resume and Quit buttons do the same thing.

  It only acts while the game's browser window is in front AND its
  title contains "cookies". Switch windows and it just waits.


--------------------------------------------------------------------
3. THE CONTROL PANEL
--------------------------------------------------------------------
  - Status line: state + current task.
  - Pause/Resume, Quit.
  - Toggles: Buy upgrades / Bonus cookies / Effects + HUD.
  - Session hours and Clicks/s boxes + Apply (1-12 clicks/s,
    0.25-72 h).
  - Stats: cookies, per sec, per click, purchases (run / all-time),
    uptime, how much the brain has learned.
  - Live log (last 300 lines).
  - "Forget what it learned": wipes the brain back to its base
    training (asks first).
  - "Check for updates": checks GitHub right now (section 16).
  Moving the mouse to the panel pauses the bot, so the button will
  say Resume. Pressing it switches back to the game, and you get
  2.5 s to let go.


--------------------------------------------------------------------
4. THE HUD (lower-left of the screen)
--------------------------------------------------------------------
  STATE     green RUNNING / amber PAUSED, WAITING, STARTING /
            red CAPTCHA
  TASK      what it's doing right now
  WANTS     what it's saving for, the price, a progress bar of your
            cookies toward it, and a countdown until it can afford it
  WHY       +production/s, payback time, and the upgrade type it
            thinks it is (build / bmult / clickpct / clickmult /
            global / unknown)
  ALSO      the next 3 options it considered and how much worse each
            scored than the pick
  NEXT      countdowns: buy, store rescan, bakery rescan, cookie
            count check, captcha/popup check
  STATS     cookies, per sec, per click, clicks, bought, bonus,
            uptime, real clicks/s, learned
  THOUGHTS  last 3 log lines
  PROMPT    the typing bar (section 17), your current order, or the
            bot's answer to what you typed

  The HUD sits above the taskbar. To move it: touch the mouse (the
  bot pauses), drag the HUD wherever you want, then F8 to resume.
  The spot is saved in cookiebot_hud.txt.

  Dotted path: a marching dotted line shows where the cursor is
  going. While it glides it traces the exact curve; while clicking
  the cookie it points at the store card it's saving for (with
  arrows if that card is scrolled off screen).

  Plus a green box around whatever it's reading at that moment (with
  a scan line sweeping down it), a
  blue vignette around the screen edges and a spinning ring
  around the cursor that bursts on each click (bigger burst for bonus
  cookies). All of it is click-through and excluded from the bot's
  own screenshots, so it never confuses the OCR. On Windows builds
  without capture exclusion, it hides them for ~40 ms during each
  capture instead.


--------------------------------------------------------------------
5. HOW IT PLAYS, STEP BY STEP
--------------------------------------------------------------------
  Main loop (main() in main.cpp), every pass:
   1. Paused / countdown / game not in front?  -> wait.
   2. Layout unknown? -> screen check (captcha/popup), then OCR the
      whole monitor to find "Store", "Your bakery", "/click", the
      footer. Builds rectangles for the left panel, the bakery, the
      store and the overall game area. Finds the big cookie by color.
   3. Every 15 s: screen check for a captcha (wait) or popup (close).
   4. Prices only change when something is bought, so the whole
      store is only scanned at start and then once an hour (to catch
      newly unlocked items). Each scan reads every item (name, price,
      owned count, research description) and hovers each building to
      read its tooltip ("each grandma makes 68.5 cookies per second",
      "each mini clicker adds 16 cookies to every click"), which gives
      exact values. It also reads the bakery panel.
      After every buy it re-reads just that store page and keeps the
      new prices in memory. After a research buy it re-reads the
      bakery panel only (multipliers changed).
      A separate thread re-reads the real cookie count every second
      (it never holds up clicking) and keeps the last value.
   5. Runs the planner on its saved prices. If it can afford the
      best plan, it buys. Otherwise it clicks the cookie until about
      when it expects to afford it (2-10 s at a time), then re-plans.
   6. While clicking it checks for bonus cookies every 0.4 s.

  Clicking: exactly 12/s (1/12 s + 0.4 ms between clicks so a strict
  1-second window never sees 13). Small +/-5 px spread on the cookie.
  Moves between different targets glide with easing along a slight
  curve.

  Buying (buy() in main.cpp):
   - Scrolls the store straight to where it last saw the item (from
     where the store already is, not from the top) and OCRs that page
     to find it.
   - If the on-screen price differs from the saved price, it updates
     the saved price. If it can't afford the real price, it re-plans.
   - Clicks the spot that has worked best before (name, price, card
     center or card edge).
   - Re-reads the same page. Success = the owned count went up, or
     the research item vanished, or (if the count is unreadable) the
     cookie count dropped by about the price.
   - If it failed and you really couldn't afford it, it just waits.
     If it failed and you could afford it, it tries the next-best
     click spot. If that fails too, it skips the item for 3 min.
     5 failed buys in a row = just clicking for 10 min.
   - If an owned count went DOWN, the store is in Sell mode. It
     pauses and tells you.
   - After a success it updates prices and counts from that page,
     and measures what the purchase actually did (see section 7).

  Prices only change when something is bought, so it plans from its
  saved price list and only fixes it when the screen disagrees.


--------------------------------------------------------------------
6. READING THE SCREEN (OCR)
--------------------------------------------------------------------
  - Uses Windows' built-in OCR engine (Windows.Media.Ocr) through raw
    COM calls. Nothing extra to install.
  - grab() screenshots a rectangle with GDI (scaled up 1.5-2x with
    smoothing, because the OCR is better on bigger text).
  - ocrRect() OCRs a rectangle and tiles it if it's bigger than the
    OCR engine's max size. alsoInverted=true OCRs a color-inverted
    copy too (used for the bakery's white-on-dark pixel font).
  - segment() (logic.h) groups OCR words into phrases on the same
    line.
  - Number parsing handles "1,234.5", "66.78 million", "1.40 billion",
    "/ sec" with spaces, "98,56" (period read as comma) and common
    OCR letter/digit swaps (O->0, l->1, S->5, B->8).
  - Prices with a decimal but no unit word ("1.15") are rejected as
    misreads, because the game always writes "1.15 billion".


--------------------------------------------------------------------
7. THE BRAIN: WHAT IT LEARNS (cookiebot_brain.txt)
--------------------------------------------------------------------
  Base training (built in, before any learning):
    - Building prices rise ~15% per purchase.
    - Upgrade wording: "x2/twice/double" + a building name =
      building doubler. "N% of your cookies per second" = per-click
      bonus. "clicks x2" = click multiplier. "+N% production" =
      global. Anything else = "unknown", assumed worth ~3%.
    - Buildings never seen are estimated at 60% of the cost-efficiency
      of the priciest building it knows.

  Learned while playing (and saved between runs):
    U  per-building cookies/sec per unit. From the bakery panel, or
       measured as the production change after buying one.
    G  per-building price growth. Measured new price / old price
       after each buy.
    C  correction factor per upgrade type: actual gain / predicted
       gain after each research buy. Applied to future predictions.
    W  word weights for "unknown" upgrades: average real gain of
       upgrades whose descriptions used that word.
    K  click-spot stats: which part of a store card actually buys it
       (successes / attempts).
    N  scroll notches per store page (adjusted so a page moves 30-75%
       of the view).
    S  wait after scrolling (ms). Goes up 100 ms whenever it can't
       find an item after scrolling.
    O  seconds of not-clicking each purchase costs (measured). The
       planner charges this to every buy, which is what stops it
       making lots of tiny purchases.
    P  lifetime counters: purchases, failed buys, bonus clicks,
       bonus clicks that boosted production.

  It skips learning from a purchase if a bonus cookie was clicked in
  the last 40 s (a boost would make the measurement wrong).
  The file is plain text, one fact per line. You can edit or delete
  lines by hand, or delete the whole file to reset it.


--------------------------------------------------------------------
8. THE PLANNER: HOW IT DECIDES WHAT TO BUY (plan() in logic.h)
--------------------------------------------------------------------
  - It builds a model of the game: cookies, cps, per-click, cookies
    per unit for each building, and the effect of each upgrade.
  - For every candidate purchase X, it runs a simulation (rollout()):
    buy X first, then keep making the greedy best-payback purchase,
    for 4 hours (or whatever is left of the session), plus 1 hour of
    "tail" value. Each simulated buy costs the learned overhead (O).
    The simulation skips anything that can't be afforded in time or
    won't pay for itself before the end.
  - The candidate with the most cookies at the end wins. That's how
    it decides between saving up for something big and buying cheap
    stuff on the way.
  - Never saves for anything it can't afford before the current
    session ends (session = --hours, rolling over every N hours,
    minimum 20 min window).
  - Production counts clicking too: cps + per-click x the click rate
    it actually achieved this run (scans and buys mean it's below 12).
    The cookie count between reads is estimated from cps x time plus
    per-click x the real number of cookie clicks, so it doesn't run
    ahead of the game's counter.
  - Sanity check: the bakery's per-building /sec numbers must add up
    to the real cookies/sec. If they're off by more than 1.5x (OCR
    missed a "million" etc.) they all get rescaled to match, and the
    log says "building readings only add up to ...".


--------------------------------------------------------------------
9. BONUS COOKIES, POPUPS, CAPTCHAS, CONNECTION LOSS
--------------------------------------------------------------------
  Bonus cookies: every 0.4 s it screenshots the game area at half
  size and looks for cookie-colored blobs (brown/golden) that are
  roundish, 14-190 px, and outside the store, bakery and big cookie.
  Blobs that were already on screen when the layout was found are
  ignored as decoration. A spot clicked 3 times in 20 s without
  disappearing gets ignored too. It notes whether production jumped
  after a bonus click.

  Popups: "Welcome back", "New cookie unlocked" and similar get
  closed by clicking a button like Collect/Nice/OK/Continue/Got it/
  Close (never one inside a danger zone).

  Captcha ("Quick check: are you a real baker?", "captcha", "not a
  robot", etc.): it stops touching the page, beeps every 30 s, and
  carries on once it's gone. It does not dismiss it. That's on you.

  Connection loss: if the game window is in front but unreadable,
  and the address bar still shows the game's site (or the title is
  an error page), it presses F5 every 10 s until the game is back.
  If you just switched to another tab, it waits instead.


--------------------------------------------------------------------
10. SAFETY RULES (what it will never click)
--------------------------------------------------------------------
  - No clicks outside the game panels (tabs, address bar, header and
    footer are off limits).
  - Never clicks text that says: reset, shop, cart, visit, rewards,
    collection, sell, netlify, delete, logout/log out/sign out, wipe,
    ascend, prestige, fresh start, checkout, address, claim.
  - The only key it ever presses is F5 (refresh). It can't close a
    tab.
  - Every click re-checks: not paused, game window in front, title
    contains "cookies", point is safe.


--------------------------------------------------------------------
11. FILES IT CREATES (next to the exe)
--------------------------------------------------------------------
  cookiebot_log.txt     everything it decided and did (appends)
  cookiebot_debug.txt   raw OCR text and parsed items for every store
                        page and buy attempt. Wiped at each start.
                        Send this when something's wrong.
  cookiebot_brain.txt   what it has learned (section 7)
  cookiebot_hud.txt     where you dragged the HUD to
  CookieBot.exe.old /.new   leftovers from an update, deleted on start


--------------------------------------------------------------------
12. SOURCE CODE MAP
--------------------------------------------------------------------
  logic.h   (portable C++, no Windows code, testable on any PC)
    Word / Seg / Rect / Layout / Econ / Item / BakeryLine  data types
    lower, lettersOnly, split          text helpers
    segment()                          OCR words -> phrases
    unitMult, parseNum, findAmount,
    isPrice, isInt                     number reading
    findLayout()                       finds the panels from OCR text
    readEcon()                         cookies, /sec, /click
    parseStore()                       store items + research
    parseBakery()                      "GRANDMA x 91" + "17,792/SEC"
    struct Brain                       everything learned + save/load
    understand()                       upgrade text -> Effect
    effGain / applyEff                 what an Effect does to the model
    rollout()                          simulate a buying strategy
    plan()                             pick the best next purchase
    makeState()                        build the model from readings

  main.cpp  (Windows)
    config + globals                   top of file
    HudState / setTask / logf          status text and logging
    Overlay: Canvas, renderVignette,
      overlayThread                    vignette, cursor effects, HUD window
    Input: sendMouse, glideTo,
      safeClick, safeWheel, pressF5    all mouse/keyboard output
    mouseHook / inputThread            pause on touch, F8/F9
    grab()                             screenshots
    WinRT OCR structs, ocrInit,
      ocrImage, ocrRect                Windows OCR
    updateDanger()                     never-click zones
    cookieColor, findBlobs,
      locateMainCookie, spawnedCookies bonus cookie / big cookie vision
    readEconNow, bankNow               cookie count + running estimate
    clickSpawned                       bonus cookie clicking
    detectLayout, connectionWatch      finding the game, F5 on failure
    screenState, dismissPopup,
      screenGuard                      captcha wait / popup close
    readStorePage, fullScan            store + bakery scanning
    clickSpots, findOnStore, buy       purchasing + learning
    renderHud                          lower-left HUD drawing
    drawButton, panelProc, panelThread the control panel
    clickFor                           cookie clicking loop
    main()                             startup + main loop


--------------------------------------------------------------------
13. HOW TO CHANGE THINGS
--------------------------------------------------------------------
  Search for the quoted text in the file named to find each spot.

  CLICKING
  - Default clicks/sec and session length: main.cpp, top:
        static double cfgCps = 12.0, cfgHours = 8.0;
  - Exact gap between clicks: safeClick():
        double gap = 1.0 / std::min(12.0, cfgCps) + 0.0004;
    The 12.0 there is the hard cap. The +0.0004 s keeps it just under
    the cap.
  - Spread on the big cookie: clickFor():
        L.cookieX + (rand() % 11 - 5)   -> 11/-5 means +/-5 px
  - Glide speed/curve: glideTo():
        dur = min(0.26, max(0.07, d / 3800.0 + 0.06))   seconds
        amp = d * 0.08                                   curve size
    Under 25 px it jumps instead of gliding.

  TIMERS (main loop in main() unless noted)
  - Full store rescan:    "now() - gLastFull > 3600"    (seconds)
  - Cookie count re-read: "now() - gEconTime > 15"
  - Bakery rescan:        fullScan(): "now() - gLastBakery < 300"
  - Captcha/popup check:  screenGuard(): "< 15"
    (also update the matching numbers in renderHud's NEXT row:
     "gLastFull + 3600", "gEconTime + 15", "gLastScreenCheck + 15")
  - Tooltip wait after hovering: readTip(): "nap(280)"
  - Refresh interval when the game won't load: connectionWatch():
        "now() - gLastRefresh < 10"
  - Bonus cookie check while clicking: clickFor(): "nextSpawn = now() + 0.4"
  - Click burst length between re-plans: main(), "clickSecs":
        min 2.0, max 10.0 seconds
  - Skip time after a failed buy: buy(): "gBlack[cur.key] = now() + 180"
  - "Just click" time after 5 fails: "gBuyPauseUntil = now() + 600"

  PLANNER (logic.h)
  - Look-ahead window and tail: plan():
        double T = std::min(sessionLeft, 4 * 3600.0), tail = 3600;
    Bigger T or tail = more willing to save for big things.
  - Minimum session window: main(): "std::max(1200.0, sess - ...)"
  - Purchase overhead starting value: struct Brain: "buyOverhead = 4.0"
    (it's learned after that; raise it to make it save up even more).
  - Price growth guess: Brain::growthFor(): "1.15"
  - Unknown upgrade guess: Brain::predictUnknown(): "0.03" (= 3%)
  - Unknown building guess: makeState(): "it.cost * eff * 0.6" and
    "it.cost / 1800.0"
  - Learning speed: Brain::learnCorr (0.7/0.3), learnGrowth (0.6/0.4),
    learnUnit (0.5/0.5). Larger second number = adapts faster.

  UNDERSTANDING NEW UPGRADE WORDING (logic.h, understand())
  - Add phrases to the x2/x3/x4 checks, e.g. to treat "quadruple" as
    4x:
        if (... || d.find("quadruple") != std::string::npos) mult = 4;
  - To add a whole new effect type: add a value to enum EffKind and
    its name to Effect::type(), detect it in understand(), give its
    gain in effGain() and its effect on the model in applyEff().

  READING THE SCREEN
  - OCR upscale: the 2.0 in ocrRect(L.store, 2.0) / ocrRect(r, 2.0).
    Higher = better on tiny text but slower.
  - Number words: unitMult() names[] list (thousand ... undecillion).
  - Layout anchors: findLayout() looks for "store", "your bakery",
    "/click", "reset my game" / "is inspired". If the site renames
    those, change them there.

  BONUS COOKIES (main.cpp)
  - Color test: cookieColor(). Thresholds on r/g/b.
  - Size/shape: spawnedCookies(): "w < 14 || h < 14 || w > 190",
    aspect "ar < 0.6 || ar > 1.65", fill "0.35".
  - Ignore after N tries: clickSpawned(): "++t->hits > 3".

  POPUPS / CAPTCHA (main.cpp)
  - Captcha phrases: screenState() cap[] list.
  - Baker puzzle: screenGuard(), "530" = click count, "done % 12 == 11"
    = how often it re-checks whether the puzzle is gone.
  - Popup phrases: screenState() pop[] list.
  - Buttons it may press on popups: dismissPopup() ok[] list.
  - Beep interval while waiting: screenGuard(): "now() - lastBeep > 30".
  - Re-check while waiting on a captcha: screenGuard(): "Sleep(1000)".

  SAFETY
  - Never-click words: updateDanger() bad[] list. Add words to block
    more buttons.
  - Allowed click area: pointSafe(). It must be inside L.game, on the
    game's monitor, and outside every danger box.

  LOOKS
  - Vignette: renderVignette(). R = width of the glow
    (0.11 x screen), 0.62f = strength, (35,120,255)-ish = color.
  - Cursor effect: overlayThread(). FXS = window size, ring radius
    30, "t * 3.2f" = spin speed, burst life 0.35 s / 0.8 s (bonus),
    maxR 46 / 110.
  - HUD: renderHud(). Size "470 * k, 358 * k", colors cyan/white/
    dim/lab, row y positions in the text() calls.
    Position: last line, "gMon.left + 26*k, gMon.bottom - H - 26*k".
    Change those to move it to another corner.
  - Panel: panelThread() (window size 452x562, colors), panelProc
    WM_CREATE (control positions), drawButton() (button colors).

  ADDING A PANEL BUTTON
  1. Add an ID to the enum above drawButton().
  2. Create it in panelProc WM_CREATE with
     mk(L"BUTTON", L"Label", BS_OWNERDRAW, x, y, w, h, ID_NEW);
  3. Handle it in panelProc WM_COMMAND.
  4. If it's a toggle, add it to the "toggle" checks in drawButton().
  (You may need to make the window taller in panelThread: RECT r.)

  ADDING A HUD LINE
  In renderHud(): text(font, color, x, y, "string"). x and y are in
  unscaled pixels (auto-scaled by k). Raise H if you run out of room.
  To show new info from the bot, add a field to HudState, set it
  under gHud.mu's lock where it happens, and read it in renderHud's
  snapshot block.


--------------------------------------------------------------------
14. HOW TO REBUILD THE EXE
--------------------------------------------------------------------
  On Windows with MSYS2 (mingw-w64):
      pacman -S mingw-w64-x86_64-gcc
      windres cookiebot.rc -O coff -o cookiebot.res
      g++ -O2 -s -std=c++17 -static -mwindows -o CookieBot.exe main.cpp cookiebot.res ^
          -lruntimeobject -lwinmm -lole32 -luser32 -lgdi32 -lwininet

  Cross-compiling from Linux:
      x86_64-w64-mingw32-windres cookiebot.rc -O coff -o cookiebot.res
      x86_64-w64-mingw32-g++-posix -O2 -s -std=c++17 -static -mwindows \
          -o CookieBot.exe main.cpp cookiebot.res \
          -lruntimeobject -lwinmm -lole32 -luser32 -lgdi32 -lwininet

  The icon is cookiebot.ico (made by icon_make.py), embedded through
  cookiebot.rc.
  Drop -mwindows to get a console window back (useful for printf
  debugging). logic.h compiles with any normal g++, so you can unit
  test the parser and planner on Linux/Mac.


--------------------------------------------------------------------
15. TROUBLESHOOTING
--------------------------------------------------------------------
  "couldn't read cookie count" over and over
      -> check cookiebot_debug.txt "econ read FAILED". It shows the
         exact OCR text. Fix the matching in readEcon() (logic.h).
  Clicks but never buys
      -> debug file: are there ITEM lines under "store @N notches"?
         If not, parseStore() isn't recognizing the cards.
  Buys the wrong thing / misclicks
      -> debug file "BUY ... via <spot>" lines. Delete the K lines in
         cookiebot_brain.txt to reset click-spot learning.
  Keeps re-finding the layout
      -> the zoom changed, or "Store" / "Your bakery" aren't readable.
  Effects show up in its screenshots
      -> old Windows without capture exclusion. Run with --nofx.
  Weird decisions after a game update
      -> Forget everything it learned (panel), or delete
         cookiebot_brain.txt.


--------------------------------------------------------------------
16. AUTO-UPDATE
--------------------------------------------------------------------
  At startup, every hour, and when you press "Check for updates"
  (or type "update"), it reads version.txt from
  github.com/binjin-maker/cookie-cwicker (main branch). If that number
  is higher than its own VERSION, it downloads CookieBot.ex_ from the
  repo, saves the brain, swaps itself out and restarts with the same
  options. The brain, log and settings files are kept. If the
  download fails or isn't a real exe, it keeps running the old one.

  To ship an update:
   1. Bump "static const int VERSION = N;" at the top of main.cpp.
   2. Rebuild, rename to CookieBot.ex_.
   3. Put N in version.txt and push both to main.


--------------------------------------------------------------------
17. PROMPT BAR (TYPED ORDERS)
--------------------------------------------------------------------
  While the bot is running, just start typing a letter. Keys go into
  the prompt bar at the bottom of the HUD instead of the game. Enter
  sends it, Esc closes it, Backspace edits. It closes on its own
  after 30 s of no typing. It doesn't chat back - it just does it,
  and shows a one-line note of what it did.

  Things it understands (wording is loose, small typos are fine).
  If you order something it can't see in the store yet (or the store
  hasn't been read yet), it rescans and keeps the order for 20 min,
  buying it as soon as it shows up.
    buy 10 grandmas / save up for a dough portal / get 2 bakeries
        -> becomes its goal. It saves up and buys only that until
           it has bought that many (counted per successful buy),
           then goes back to normal play. Works for research
           too. Number words (one..twenty) work.
    cancel / nevermind          drop the current order
    skip bakery / don't buy X   never buy that ("allow X" undoes it)
    stop buying / start buying  turn buying off / on
    bonus off / bonus on        bonus cookie clicking
    hide hud / show hud         effects + HUD
    clicks 8                    clicks per second (1-12)
    6 hour session              planning window
    rescan                      re-read the store and bakery now
    update                      check GitHub for a new version
    pause                       pause (F8 to resume)
    help                        list of examples

  Code: parseOrder() in logic.h (testable on Linux), processCmds()
  and checkGoal() in main.cpp, keyHook() for the typing.


--------------------------------------------------------------------
18. COPYCAT (WATCH YOU PLAY ANY GAME, THEN PLAY IT)
--------------------------------------------------------------------
  Works on any windowed / borderless game (true exclusive fullscreen
  often captures black - use borderless windowed).

  F6 (or "Watch me play")  click the game, press F6, play normally.
      It records the game window 30 times a second (tiny 32x18 snapshot
      + colour), every key you hold, mouse buttons and raw mouse
      movement (works with locked FPS cursors). F6 again stops & saves.
  F7 (or "Let it play")    it plays: it finds the remembered moment
      that looks most like the screen (favouring moments that turned
      out well) and replays what you did from there for up to ~0.8 s,
      then re-checks. Following whole moments instead of averaging
      many keeps its movement smooth instead of jittery.
  F10   tell it something any time (it pauses while you type):
      "don't press e" / "never click"   -> rule, it won't press it
      "you can press e"                 -> undo a rule
      "this is the base" / "this is a cave"  -> names the current place
      "this is dangerous" / "this is good"   -> marks this moment
      "red ones are enemies" / "green ones are friendly"
                                        -> colour of moving things
      "3 is the hatchet"                -> names hotbar slot 3
      "bears are dangerous" / "people are enemies" / "cows are food"
                                        -> what detected things are
      "that's a tree" / "looking at a rock" -> names what's in the middle
      or an action ("run forward", "shoot", "turn around").
      Rules/names are saved in copycat\<game>.rules.

  SAFETY while it plays:
   - never presses Windows/Alt/Esc/F-keys/Delete/media keys
   - each key/button changes at most ~6 times a second (no click spam)
   - mouse turns are capped per frame
   - if the game window stops being in front, or a visible mouse
     pointer leaves the game window, it lets go of everything and
     stops playing completely
      Touch the mouse/keyboard any time and it instantly hands over
      and records you instead; 2 s after you let go it takes back over.
      F7 again stops.

  It keeps learning while it plays alone: its own frames are stored
  too, and whatever happens after them (good or bad) changes how
  much it trusts that move next time. ~4% of the time it tries a
  small variation to discover things.

  Good / bad: it reads numbers on the game screen ~5x a second and
  finds the label next to each one. Health/armor/food/water/stamina
  going up = good, down = bad; score/kills/coins/wood/stone/metal/
  scrap/loot up = good; radiation/cold/poison up = bad; ammo, time,
  fps and coordinates are ignored; numbers that tick constantly
  (clocks) are ignored. A sudden red flash of the screen = bad.

  Needs: if health/food/water/etc. gets low or keeps dropping, it
  favours moments that raised that stat before ("trying to get").

  Object detector (real trained AI): YOLOX-S, a neural network
  trained on 80 everyday things - people, cars, trucks, boats,
  planes, bikes, dogs, horses, cows, bears, birds, backpacks,
  bottles, knives, chairs... It runs on your GPU through ONNX Runtime
  + DirectML (CPU if the GPU won't work), several times a second, and
  draws labelled boxes over the game (red = enemy, green = friendly,
  yellow = unknown). Whatever it sees right before bad/good events
  gets marked enemy/friendly ("persons show up before bad stuff ->
  probably enemies"); with an enemy ahead it favours moments where
  you were shooting. The first run downloads ~75 MB of extra files
  next to the exe (onnxruntime.dll, DirectML.dll, yolox_s.onnx); the
  SEES row on the HUD shows its status. It only knows those 80
  everyday classes - game-specific things (ore nodes, hatchets)
  come from the next two features.

  "What am I looking at": it keeps a small picture of the middle of
  the screen. When you hit something and a stat goes up right after
  (wood, stone...), it remembers what that thing looked like as
  "gives wood". F10 "that's a tree" / "looking at a crate" teaches it
  by name. When it needs wood and is looking at a "gives wood" thing,
  it hits it.

  Moving things: each frame it removes the camera's own motion and
  finds what is still moving (players, NPCs, animals, vehicles),
  with colour and shape (tall = person-like, wide = animal/vehicle-
  like). Colours that show up right before bad events become
  "probably enemies"; with an enemy-looking thing ahead it favours
  moments where you were shooting.

  Tools: when you press 1-9 it learns what the hotbar looks like with
  that slot picked, so it can tell what you're holding. It notes
  which tool raised which stat ("slot 2 gets wood") and when it needs
  that stat it switches to that tool itself.

  Places: it groups what the screen looks like into up to 64
  "places" (named by colour, e.g. "Place 4 (mid green)") and notes
  which stats went up/down at each place - e.g. "wood goes up most
  at Place 3".

  Confused: if nothing it remembers looks like the screen for 1.5 s,
  it lets go of everything, beeps and asks "What are you doing?" on
  the HUD. Type an answer and Enter:
     "running forward", "shooting", "turning around", "looting",
     "jump over", "crouch and sneak left", "drink water", "wait 5"
     -> it does that for a few seconds (number = seconds)
     "let me show you" / Esc -> it watches you for 15 s
  The answer is remembered with that screen; next time it sees
  something similar it just does it without asking.

  Analysis HUD (lower left while the game is in front): mode, speed
  (fps, decide ms, number-reading ms), what numbers it's watching,
  which place it's in, what it has worked out about each key/mouse
  (moves you, turns the view, good/bad after), precision (slowing
  aim before clicks), danger colours, what it's trying to get, what
  it's pressing, how confused it is, and the last notes it wrote.

  Files (in copycat\ next to the exe, one set per game exe name):
     <game>.rec            everything it recorded + places + answers
     <game>_journal.txt    readable notes, events and findings
  Code: copycat.h.
