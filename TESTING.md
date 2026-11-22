# End-to-end test scenario

A scenario for Claude (or anyone) to run with `linux/vitactl.py`, to check that
everything works together: video, sound (plugin build 101+), input, and
VitaShell's FTP server running alongside them. Each step has **do** (the commands), **pass** (what must be
seen) and **record** (what goes in the report). If a step fails, stop there and
report.

## Before you start

- The plugin is running: `lsusb` shows `054c:1337`. USB transfer is closed,
  Wi-Fi is on, and the Vita shows LiveArea.
- The values come from `.claude/vita-testing.md`, **Connection**:

  ```bash
  VITACTL=...; VITA_IP=...; FTP_PORT=...   # from .claude/vita-testing.md
  FTP="ftp://$VITA_IP:$FTP_PORT"
  DEST=/tmp/sync-vita-scenario-$(date +%Y%m%d-%H%M); mkdir -p "$DEST"
  ```

- Every shot goes to `$DEST`, numbered: `$VITACTL shot "$DEST/01-<what>.png"`.
  Look at each one before the next input.
- **This scenario sends input.** For the rest of the boot, files go over FTP
  only: opening USB transfer can freeze the Vita (see [TODO.md](TODO.md)).
- VitaShell is opened through the **VitaDeploy** app: VitaDeploy > File
  Manager. While its FTP server runs in the background, PS is locked until
  it's closed: SELECT brings the dialog back, and ○ Cancel disconnects.

## 1. Video

- **Do:** `$VITACTL watch start` (one attempt; if it fails, follow its
  advice), then `$VITACTL watch status` and a shot.
- **Pass:** the last frame is under 1 s old; `file "$DEST/01-video.png"` says
  960 x 544; the shot shows LiveArea.
- **Record:** frame age, what the shot shows.

## 2. Sound

- **Do:** `$VITACTL watch stop`, then `$VITACTL watch start --audio`, and
  `$VITACTL watch status`. Then, from LiveArea:

  ```bash
  printf '%s\n' "wait 600" "press VOLUP" "wait 1000" "press VOLDOWN" "wait 1000" \
      "touch 705 150" "wait 2000" "press PS" "wait 800" > "$DEST/sound-steps.txt"
  $VITACTL record 6 "$DEST/sound.mkv" --before 1 & sleep 0.3
  $VITACTL run "$DEST/sound-steps.txt"; wait
  $VITACTL analyze "$DEST/sound.mkv"
  ```

  If the Vita's volume is up, ask the user to listen to it during this. Then
  `$VITACTL watch stop` and `$VITACTL watch start` (without sound) for the
  next steps.
- **Pass:**
  - `watch status` says "with sound";
  - the recording says "with sound";
  - `analyze` shows a sound section that isn't "silent all along", and a
    "sound reacts" figure per input; `audio.png` shows the waveform;
  - if the Vita's speaker volume is up, the user hears it go silent while
    sound is captured, and come back after `watch stop` (the capture itself
    doesn't depend on the volume: it's taken before it).
- **Record:** the levels, the sudden sounds, the reaction times. If the Vita
  had gone to sleep (lock screen, frozen picture, silence), unlock it and
  start the step again.

## 3. Inputs

- **Do:**
  1. `$VITACTL swipe 480 450 480 100` (next page, pages scroll vertically),
     then a shot; `$VITACTL swipe 480 100 480 450` (back), then a shot.
  2. `$VITACTL press VOLUP`, then a shot right away; `$VITACTL press VOLDOWN`.
  3. `$VITACTL status`.
- **Pass:**
  - the swipes change the LiveArea page, then bring it back;
  - the volume bar shows after VOL+;
  - `status` shows no button held, and front and rear touch up.
- **Record:** each check, and the plugin's build number from `status`.

## 4. VitaDeploy > File Manager

- **Do:** find VitaDeploy's bubble in a shot (`swipe` between LiveArea pages
  if it isn't there; on the 2026-11-22 run it was in the **Système** folder), `touch` it, then start it (`touch` the start button or
  `press CROSS`). In VitaDeploy, go to **File Manager** (the moves read from
  the screen: `press UP`/`DOWN`, `press CROSS`).
- **Pass:** a shot shows VitaShell's file list (title "VitaShell" top left).
- **Record:** how VitaDeploy's menu was navigated.

## 5. Set SELECT to FTP

- **Do:** `$VITACTL press START` (VitaShell's settings), then a shot. Move the
  highlight to **SELECT button** (`press DOWN`/`UP`, a shot after each). If it
  says USB, `press CROSS`. Leave the settings (the key the screen shows, else
  `press CIRCLE`).
- **Pass:** a shot shows SELECT button set to FTP; then a shot shows the
  file list again.
- **Record:** what it was set to before. It stays on FTP; say so in the report.

## 6. FTP in the background

- **Do:** `$VITACTL press SELECT`, then a shot. `$VITACTL press CROSS` (OK,
  keep it in the background), then a shot.

  ```bash
  curl -sS --connect-timeout 5 "$FTP/ux0:/" | head -5
  $VITACTL watch status
  ```

- **Pass:**
  - the dialog shows `ftp://$VITA_IP:$FTP_PORT`;
  - after OK, the dialog is gone and an icon shows left of the clock;
  - `curl` lists the card;
  - the last frame is still under 1 s old.
- **Record:** the address shown, the listing's first lines.

## 7. Send the test file

- **Do:**

  ```bash
  curl -sS --fail -T testing/ftp-test.txt "$FTP/ux0:/downloads/ftp-test.txt"
  size=$(curl -sSI "$FTP/ux0:/downloads/ftp-test.txt" | tr -d '\r' | sed -n 's/^Content-Length: //p')
  echo "Vita: $size, PC: $(stat -c %s testing/ftp-test.txt)"
  ```

- **Pass:** both sizes match (161 bytes).
- **Record:** the sizes.

## 8. Read it with vitactl

- **Do:**
  1. In the File Manager, go to `ux0:downloads/`. The list doesn't refresh by
     itself after an FTP change: if you're already there, `press CROSS` on
     `..` and `press CROSS` on `downloads/` to come back in.
  2. Highlight `ftp-test.txt`, then `$VITACTL press CROSS` to open it, then a shot.
  3. `$VITACTL press CIRCLE` to go back.
- **Pass:** the shot shows every line of
  [testing/ftp-test.txt](testing/ftp-test.txt), in particular
  `Marker: VITACTL-FTP-7F3A`.
- **Record:** the text as read from the shot, and any line that differs.

## 9. Clean up

- **Do:**

  ```bash
  curl -sS -Q "DELE /ux0:/downloads/ftp-test.txt" "$FTP/" > /dev/null
  curl -sS "$FTP/ux0:/downloads/"
  ```

- **Pass:** the listing no longer shows `ftp-test.txt`.
- **Record:** that it was deleted.

## 10. Close FTP

- **Do:** `$VITACTL press SELECT` (step 5 set it to FTP): the FTP dialog
  comes back (shot). `$VITACTL press CIRCLE` (Cancel, disconnect), then a shot.

  ```bash
  curl -sS --connect-timeout 5 "$FTP/ux0:/" > /dev/null && echo "FTP STILL OPEN" || echo "FTP closed"
  ```

  Then `$VITACTL press PS`, a shot, and `$VITACTL watch stop`.
- **Pass:**
  - the icon by the clock is gone;
  - `curl` prints "FTP closed";
  - PS reaches LiveArea, which shows PS is unlocked.
- **Record:** each check.

## 11. Report

A short message to the user:

- a table: step, result (pass, fail or skipped), what the shot showed;
- the folder with the shots (`$DEST`), and the shots that matter;
- anything odd: slow or stale frames, an unexpected dialog, a slow FTP reply;
- the end state: SELECT is set to FTP, and this boot is FTP only.
