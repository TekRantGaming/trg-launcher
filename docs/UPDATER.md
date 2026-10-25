# The updater (`trg/updater.h`)

The updater from King Kong Recompiled, made general: the game passes its GitHub repos, its version, its program
folder and the paths that belong to the player, and the launcher gets an Updates page that checks, installs and
restarts. This page is the why behind each rule. INTEGRATION.md section 15 shows the wiring.

## The first updater you ship is the one that matters

Whatever the updater in players' hands cannot do, you can never fix for the players who update with it. King Kong's
first updaters copied only the root files and two named folders, so every folder added later (the startup movie, the
built in mods, a whole extra feature) silently never arrived for those players. A release had to be pulled back, and
every release since carries workarounds for those old updaters.

So put the full updater in your FIRST public release, before you know you need it:

- **Install every file the release lists.** No folder list in the code. New folders just work.
- **Keep every folder the release has.** Never flatten or skip one because "it never changes".
- **Ship the file list in the program folder's root**, so even a simple updater copies it and the new version can see
  what should be there.
- **Check the folder at the first start of every version** and fetch whatever is missing. This is the one hook that
  reaches players who updated with an older, weaker updater: after any update, the NEW program runs.

## What it does

1. **Check.** Reads the last 20 releases of the stable repo (and the beta repo on the beta channel) and takes the
   newest version that has this platform's asset, skipping drafts. Not `/releases/latest`: that goes by date,
   not version, so a hotfix for an older line published last would win. Versions compare as numbers, and a beta comes
   before its release: `2.0.0-beta.2 < 2.0.0-beta.10 < 2.0.0`. `beta10`, `beta.10` and `beta-10` read the same.
2. **Channels.** Stable sees only stable releases. Beta sees betas, plus a stable release whose x.y.z is at least the
   running beta's (its own final release), never an older line's hotfix. Changing channel offers that channel's newest
   version even when it is older: going back to stable can mean going down a version. Nothing installs until the
   player presses Install.
3. **Asset name.** The whole name must match the pattern (`*windows-x64.zip`). King Kong's old updaters took any asset
   whose name merely contained `windows-x64.zip`, so an extra `...windows-x64.zip.json` would have broken them. Publish
   ONE asset per pattern; `tools/make_update_files.py check` fails otherwise.
4. **Verify before anything changes.** The download is checked against the release's `SHA256SUMS.txt`. A release
   without one is checked by its size only, so always attach it. Every file to install is also checked against the
   file list. A failed check changes nothing and says so.
5. **Install with undo.** Each file is first written beside its target as `<name>.new`. Then each target becomes
   `<name>.old` (Windows allows that for a running program or a loaded DLL) and the `.new` takes its name. If any swap
   fails, usually because another program holds the file (an antivirus scan, a video player, a second copy of the
   game), every old file is put back, the new folders are removed, and the player is told which file was in use. The
   file list is installed last, so until the end the old list still describes the folder. A checked download stays
   in the work folder, so trying again does not download it again.
6. **Remove only what is ours and unchanged.** Files an earlier release shipped and this one does not are removed only
   while their size and SHA256 still equal what we shipped: the retired list, plus what the installed file list named.
   A file the player edited, ReShade, a dropped in mod, anything unknown: never touched. Folders left empty go too.
7. **Never touch the player's things.** The game files, saves, settings, logs, mods, screenshots: list them in
   `protected_paths`, as paths inside the program folder or as absolute folders. An update never writes or deletes
   there, even if a release lists such a path by mistake, and the packaging check refuses such a package.
8. **Restart.** The program starts itself again with the same command line and the old one quits.
   `CleanUpAfterUpdate()` at the next start removes the `.old` files.
9. **First start check.** Every start until it passes: compares the program folder with the file list (sizes, and
   SHA256 of files up to 4 MB, so it stays quick), removes unchanged retired files and fetches what is missing or wrong
   from this version's own release. A passed check is remembered in `state_file`. A read only folder is left alone
   rather than downloading at every start.
10. **Linux.** An AppImage is one file: it is downloaded, checked, written beside itself and renamed over itself.
    A copy not running as an AppImage says to download the new version by hand. macOS has no update path yet (TODO).

## Packaging

`tools/make_update_files.py` (Python 3, standard library only):

```bash
python tools/make_update_files.py package out/MyGame --version 1.2.0 \
    --earlier releases/MyGame-v1.1.0-windows-x64.zip --protect game --protect settings.toml \
    --zip out/release/MyGame-v1.2.0-windows-x64.zip
```

It writes `update-files.txt` and `update-retired.txt` into the program folder, zips it (with the folder inside the
zip), writes `SHA256SUMS.txt` beside the zip and runs the check, which must print `PACKAGE CHECK PASSED`. The check
fails on: a zip that differs from its file list, an unsafe path, a protected path in the package, a retired file that
the release still ships, leftover `.old`/`.new`/`.part` files, a missing or wrong `SHA256SUMS.txt` line, and a second
file beside the zip matching the asset pattern. Pass `--also <AppImage>` to put the Linux asset in the same
`SHA256SUMS.txt`.

`--earlier` takes earlier release zips or file lists; an earlier zip's own retired list counts too, so the previous
release is enough once every release was made this way. Keep every release zip you publish.

Also test every update path before a release: copy an install of each older version still in players' hands, update
it to the new package, and compare the folder with the file list. Unit tests cover the rules; only that run shows what
an older updater actually leaves behind.

## Antivirus

Players who see a virus warning think the project was hacked, and a clean scan is only a snapshot.

- Upload every release zip to VirusTotal and publish only when it is clean. A local Defender scan is NOT proof: the
  `!ml` verdicts come from Microsoft's cloud check on downloads, sometimes hours after upload.
- Submit every NEW or changed DLL and exe to Microsoft before the release
  (https://www.microsoft.com/en-us/wdsi/filesubmission, "Software developer"). Flagged files were cleared within
  hours. A fresh build often clears an `!ml` verdict too.
- Attach `SHA256SUMS.txt` to every release, so players and the updater can check the download.
- Code signing is the real fix.

## Testing it

`tests/updater_test.cpp` runs on real folders with zips it writes itself and a table of fake GitHub answers, so it
needs no network: version order, release picking, file lists, an install over an old tree, undo after a failed swap
(and a file held open on Windows), retired file rules, protected paths and the first start check.
