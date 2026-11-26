---
name: vita-testing
description: Test a build of the current project on the PS Vita. Reads the project's .claude/vita-testing.md, checks the Vita is reachable over USB or FTP, builds and copies the build, then offers to run the test with sync-vita (Claude sees the screen and sends input) or tells the user what to try, and finally moves the results off the Vita (USB or FTP) to a folder in /tmp and reads them. Use when the user asks to test on the Vita, to copy or send a build to the Vita, or to fetch, move or read results from it.
argument-hint: "[flavour]"
---

# Testing a build on the Vita

Steps, in order: **0** project facts, **1** connected?, **2** copy the build,
**3** offer sync-vita, then either **4** the user tests or **5** Claude tests
with sync-vita, then **6** connected again? and **7** bring the results back.

## Facts common to every project

- `$MOUNT`, `$VITA_IP`, `$FTP_PORT` and `$VITACTL` come from the project
  file's **Connection** section (step 0). Below, `FTP="ftp://$VITA_IP:$FTP_PORT"`.
- **VitaShell** is opened from LiveArea through the **VitaDeploy** app:
  VitaDeploy > **File Manager**. "VitaShell" below means that.
- The Vita is reached one of two ways, called `$LINK` below:
  - **USB:** VitaShell's USB mode (SELECT in its File Manager; `lsusb`:
    `054c:05d3`) mounts the memory card, `ux0:`, at `$MOUNT`.
  - **FTP:** VitaShell's FTP server. Paths look like `$FTP/ux0:/data/...`.
    A URL ending in `/` lists a folder (`ls -l` style); the server refuses
    curl's `--list-only` (`502`), and answers `curl -sSI` with the file's
    `Content-Length`.
- **VitaShell's SELECT** (in its File Manager) opens USB transfer or the FTP
  server, whichever its settings choose, and the user switches it. The FTP
  dialog offers **✕ OK** (keep the server in the background) or **○ Cancel**
  (disconnect). While FTP runs in the background, a page-with-arrow icon
  shows left of the clock, and **PS is locked**: VitaShell (and VitaDeploy
  around it) can't be left until FTP is closed: SELECT brings the dialog back, and ○ Cancel
  disconnects. VitaShell's file list doesn't refresh by itself after an FTP
  change: leave the folder and come back in. The video and FTP work at
  the same time.
- **Setting SELECT to FTP** (before Claude presses SELECT, every time):
  1. `$VITACTL press START` opens VitaShell's settings; take a shot.
  2. Find the **SELECT button** row. If it says USB, move the highlight to it
     (`press DOWN` or `UP`, a shot after each) and `press CROSS` to switch it
     to FTP.
  3. A shot must show FTP. Leave the settings (the key the screen shows, else
     `press CIRCLE`) and check in a shot that the File Manager is back.
  4. Only then `press SELECT`.

  If a shot doesn't show the row clearly, stop and ask the user. It's left on
  FTP afterwards (safer after input); say so in the report. An FTP dialog
  still on screen is closed with `press CIRCLE` instead, which is always safe.
- An installed app lives in `app/<TITLEID>/`; its program is
  `app/<TITLEID>/eboot.bin` (a build's `.self` file).
- A `.vpk` copied to `downloads/` is installed from VitaShell (✕ on it).
- **sync-vita:** `$VITACTL --help` lists the commands. The Vita running the
  VitaSync plugin shows as `054c:1337`. Closing VitaShell's USB transfer
  (CIRCLE) gives USB back to the plugin about 5 s later. The plugin can press
  PS (build 90+). After `$VITACTL watch stop`, input is lost until the next
  `watch start` or USB transfer: send input before stopping the video.
- **USB transfer freeze:** with plugin build 110 or older (`$VITACTL status`
  shows the build), opening USB transfer after the video or input was used in
  that boot freezes the Vita until it's forced off (sync-vita's `TODO.md`).
  In such a boot, use **FTP only**, unless the Vita has been rebooted since.
  From build 111 USB transfer works then; rarely, VitaShell hangs ~30 s (PS
  frees it), and after a hang while closing, the Vita hasn't re-read `ux0:`:
  have it rebooted before anything is written to `ux0:`.

## 0. Project facts

Read `.claude/vita-testing.md` at the project's root. It gives:

- **Connection:** `MOUNT` (where USB mode mounts `ux0:`), `VITA_IP` and
  `FTP_PORT` (VitaShell's FTP server), `VITACTL` (sync-vita's `vitactl.py`);
- **Folder on ux0:**: where the program reads and writes its files;
- **Flavours:** for each one, the build command, the build folder, the
  package (`.vpk`) and its `.self`, the title ID and the app's name, and
  which flavour is the default (used when `$ARGUMENTS` is empty);
- **Destination:** only if the build is not an installed app (a plugin, a
  file dropped somewhere on `ux0:`...): where it goes and how it is loaded;
- **Results:** the files and folders a run writes that are to be brought
  back, and what each one is;
- **Never touched:** saves, settings and anything else of the user's;
- **What to test from:** the docs that say what a step must show, and
  what's normal while testing;
- **Reading results:** how to read each result;
- **Keeping captures:** where a result may be kept in the project, if
  anywhere.

If the file is missing, or lacks a section, ask the user for these facts
(skip what they say doesn't apply), write them in the file under these
headings, then go on.

## 1. Connected?

Try USB, then FTP. The first that answers is `$LINK` for this run:

```bash
ls "$MOUNT/"                                # USB: expect app/ data/ VitaShell/ among others
curl -sS --connect-timeout 5 "$FTP/ux0:/"   # FTP: lists the card's folders
```

- With plugin build 110 or older, if `$VITACTL` has sent input or run
  `status` in this boot, only FTP counts (freeze).
- If neither answers, ask the user to open VitaDeploy > File Manager and
  either press
  SELECT (USB) and plug the Vita in, or start its FTP server (same network
  as the PC; the dialog should show `$VITA_IP`). Then check again.
- If a different `/media/ryan/*` folder appears, or the FTP dialog shows
  another address, ask rather than guess.

## 2. Copy the build

1. Build the flavour with the project's command. Changed files must
   compile without warnings:

   ```bash
   <build command> 2>&1 | grep -iE "warning|error" -A3
   <build command> 2>&1 | tail -3
   ```

2. Copy it:
   - an app that is installed (`app/<TITLEID>/` exists): the `.self` over
     its `eboot.bin`;
   - an app that isn't: the `.vpk` to `downloads/`;
   - anything else: to the project's **Destination**.
3. Check the copy:

   **USB:** compare, and flush it to the card:

   ```bash
   cmp <source> "$MOUNT/<path on ux0:>" && sync
   ```

   **FTP:** upload, then compare the server's size with the local one:

   ```bash
   curl -sS --fail -T <source> "$FTP/ux0:/<path on ux0:>"
   size=$(curl -sSI "$FTP/ux0:/<path on ux0:>" | tr -d '\r' | sed -n 's/^Content-Length: //p')
   [ "$size" = "$(stat -c %s <source>)" ] && echo "copy OK" || echo "SIZE MISMATCH: $size"
   ```

   FTP then stays in the background, and PS is locked: it's closed before
   the app is started (step 4 or 5).

## 3. Offer sync-vita

Every time, ask the user whether Claude should run the test itself with
sync-vita, or the user tests by hand. Never assume either answer, even if
the last test was driven; wait for the reply.

- **By hand:** step 4.
- **With sync-vita:** step 5.

## 4. The user tests

Tell the user, in a few numbered lines:

- **Close FTP** first (SELECT, then ○ Cancel, in VitaShell), if the build
  came over FTP:
  PS is locked until then.
- **Install**, if it went to `downloads/`. Name the app to start.
- **What to do:** where to go, for how long, and what to watch for. Take
  it from what the current step must show, in the docs the project file
  names.
- **What's normal**, from the project file.
- **When done:** quit the app, then in VitaDeploy > File Manager open USB
  mode and plug in,
  or start its FTP server (✕ OK to keep it in the background), and say so.
  Then step 6.

## 5. Claude tests with sync-vita

1. **Hand USB back to the plugin.** If USB transfer is still open (`$MOUNT`
   exists, or `lsusb` shows `054c:05d3`), ask the user to close it with
   **CIRCLE**. A `.vpk` in `downloads/` must be installed in VitaShell
   first. Then wait for the plugin:

   ```bash
   for i in $(seq 1 30); do lsusb | grep -q 054c:1337 && break; sleep 2; done
   lsusb | grep 054c
   ```

   No `1337` after a minute: say so and stop (the plugin isn't running, or
   needs a reboot and henlo).
2. **Video, then screen.** `$VITACTL watch start` (one attempt; if it fails,
   follow its advice, don't retry in a loop). The default 960x544 gives about
   47 frames/s; to measure a 60 fps game, `--size 864x488` (57/s) or
   `480x272` (60/s) instead (`watch stop` first to change size). When sound
   matters (plugin build 101+), add `--audio`: `record` then keeps the sound
   and `analyze` reports it. From plugin build 107 the Vita keeps playing its
   sound meanwhile (`status`: "Vita speaker"); with older builds, or if
   `status` says muted, it's silent while it's on: tell the user, and
   `watch stop` gives the sound back. Create the results folder now,
   `DEST=/tmp/<project>-$(date +%Y%m%d-%H%M)`, and save every shot there:
   `$VITACTL shot "$DEST/01-<what>.png"`. Look at each shot before the next
   input.
   **If FTP is running** (step 1 used FTP, or the shot shows its icon by the
   clock), close it now ("Setting SELECT to FTP", then SELECT, then `press CIRCLE`
   on the dialog), and check in
   a shot that the icon is gone: PS is locked until then.
3. **Start the app.** `$VITACTL press PS` to reach LiveArea if needed. Find
   the app's bubble in a shot (`swipe` between pages if it isn't on the
   first one), `touch` it, then start it (`touch` the start button, or
   `press CROSS`). Shot to confirm it's running.
4. **Run the test** the current step needs (the project file's **What to
   test from**), with `press`, `hold`, `stick`, `touch`, `swipe` and `run`.
   Say what each shot shows. If something unexpected appears (a dialog, a
   crash, an error code), stop and show it to the user. For anything shorter
   than a second (a flash, a hitch, a stutter) or to measure frame rate:
   `$VITACTL record SECONDS "$DEST/<what>.mkv" [--before S]` (in the
   background, `&`, while `run` plays the inputs), then `$VITACTL analyze`
   on it, and look at the pictures it flags rather than at every frame.
5. **When done:** quit the app (`press PS`, then close it if needed). With
   plugin build 110 or older, this boot is now **FTP only** (freeze). Open VitaDeploy from LiveArea
   (find its bubble in a shot, `touch` it, start it), choose **File
   Manager**, then start its FTP server ("Setting SELECT to FTP", then SELECT). Check in
   a shot that its dialog shows `$VITA_IP` (say so if it doesn't), then
   `press CROSS` (OK, background). Leave the video on: step 7 closes FTP.
6. Go to step 6.

## 6. Connected again?

Run step 1's check again, with the same rule: with plugin build 110 or
older, after `$VITACTL` input or `status` in this boot, FTP only. If it fails, ask for USB mode or the FTP
server again, in VitaDeploy > File Manager (for FTP: check its dialog's address, and that the Vita
and the PC are on the same network).

Step 7 doesn't start, nothing is moved and nothing deleted, until this check
passes.

## 7. Bring the results back

1. Move the results into the folder in `/tmp` (`$DEST`; create it if step 5
   didn't). Fill `SRC`, `DEST` and `RESULTS` from the project file.

   **USB:** `mv` across drives copies first and deletes each original only
   after its copy succeeded, which leaves the Vita's folder clean for the
   next run:

   ```bash
   SRC="$MOUNT/<folder on ux0:>"
   DEST=/tmp/<project>-$(date +%Y%m%d-%H%M)   # or the one from step 5
   RESULTS="<result files and folders, space-separated>"
   mkdir -p "$DEST"
   for f in $RESULTS; do [ -e "$SRC/$f" ] && mv "$SRC/$f" "$DEST/"; done
   sync
   ls -la "$DEST"; ls -la "$SRC"
   ```

   **FTP:** download each file, compare its size with the server's, and
   delete it on the Vita only when they match; folders are walked with
   their sub-folders, and removed once empty:

   ```bash
   FTP="ftp://$VITA_IP:$FTP_PORT"
   SRC="ux0:/<folder on ux0:>"
   DEST=...; RESULTS="..."
   mkdir -p "$DEST"
   url()     { printf '%s' "$FTP/$SRC/$1" | sed 's/%/%25/g; s/ /%20/g; s/#/%23/g; s/?/%3F/g'; }
   entries() { curl -sS --fail "$(url "$1")" | tr -d '\r'; }   # $1: "" or "folder/"
   name()    { sed -E 's/^([^ ]+ +){8}//'; }   # the name after "-rw-r--r-- 1 vita vita SIZE Mon DD HH:MM"
   files()   { entries "$1" | grep '^-' | name; }
   dirs()    { entries "$1" | grep '^d' | name | grep -vxE '\.\.?'; }
   fetch() {   # $1: path under SRC; moved to the same path under DEST
       curl -sS --fail "$(url "$1")" -o "$DEST/$1" || return 1
       size=$(curl -sSI "$(url "$1")" | tr -d '\r' | sed -n 's/^Content-Length: //p')
       [ -n "$size" ] && [ "$size" = "$(stat -c %s "$DEST/$1")" ] || return 1
       curl -sS -Q "DELE /$SRC/$1" "$FTP/" > /dev/null
   }
   move_dir() {   # $1: folder under SRC
       mkdir -p "$DEST/$1"
       files "$1/" | while IFS= read -r g; do fetch "$1/$g" || echo "NOT MOVED: $1/$g"; done
       dirs "$1/" | while IFS= read -r d; do move_dir "$1/$d"; done
       curl -sS -Q "RMD /$SRC/$1" "$FTP/" > /dev/null 2>&1 || echo "NOT REMOVED: $1/"
   }
   for f in $RESULTS; do
       if dirs "" | grep -qxF "$f"; then move_dir "$f"
       elif files "" | grep -qxF "$f"; then fetch "$f" || echo "NOT MOVED: $f"
       else echo "not on the Vita: $f"; fi
   done
   ls -laR "$DEST"
   ```

   If `mv` or the FTP script reports an error, say what, and leave the rest
   alone.
2. Say where the results are (the folder's full path). After USB, say the
   Vita can be unplugged. After FTP, close the server so PS works again:
   after a driven test, Claude does it ("Setting SELECT to FTP", then
   SELECT, then `press CIRCLE` on the dialog; a shot must show the icon
   gone), then `$VITACTL watch stop`; otherwise, tell the user to press
   SELECT, then ○ Cancel, in VitaShell.
3. Read them against what the step expects, as the project file says.
   Report what was found, and what it means for the step. For a driven
   test, include the shots that matter.

## Rules

- Commit nothing as part of this.
- Delete nothing on the Vita but the results moved in step 7, and only
  once each one has been copied. Never touch what the project file lists
  under **Never touched**.
- With plugin build 110 or older, never ask for USB transfer in a boot where
  `$VITACTL` has sent input or run `status`; use FTP or have the Vita
  rebooted first.
- Never press SELECT without first checking in VitaShell's settings that it
  opens FTP ("Setting SELECT to FTP"): it might open USB transfer.
- While FTP runs in the background, PS is locked; close FTP before anything
  needs PS.
- Results are never committed. To keep one in the project, it goes where
  the project file's **Keeping captures** says.
- This skill has a copy in sync-vita's `.claude/skills/vita-testing/SKILL.md`.
  Change `~/.claude/skills/vita-testing/SKILL.md`, then copy it over that
  one (`cmp` to check).
