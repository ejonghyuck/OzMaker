# OzMaker

Win32 + Direct2D, immediate-mode UI (`src/Ui.*`). Views are in `src/AppViews.cpp`; test modes are in `src/SelfTest.cpp`. The README's "테스트용 명령줄" section lists them.

## Build and test

- Build: MSBuild `OzMaker.vcxproj /p:Configuration=Release /p:Platform=x64`. Without vcpkg you get a GIF/MP4-only build; WebP/AVIF are built in CI.
- CI runs `tools/ci-selftest.ps1`, which runs `--selftest`.
- After UI changes, also run `--screenshot <dir>` and look at the PNGs. It fails on layering errors.
- Point `OZMAKER_SETTINGS` at a scratch settings file for every test run.

## Versions and releases

- **Version number:** `src/Version.h` holds it. `res/OzMaker.rc` reads it from there; `res/app.manifest` and `vcpkg.json` repeat it. `tools/package.ps1` warns when they differ; the release workflow runs `tools/check-version.ps1`, which stops when they differ or when the tag isn't `v` + that version.
- **Release assets:** every release carries the zip, `OzMaker.exe` and `update.txt`. Keep those names. `OzMaker-<version>-pdb.zip` goes with them, for reading crash reports (`src/Crash.h`). Installed copies look for `releases/latest/download/update.txt` and the exe it names (`src/Updater.h`).
- **Updater tests:** `--update-test <dir>` (also part of `--selftest`) uses a fake release folder through `OZMAKER_UPDATE_URL` and a copy standing in for the exe. Never point it at the running exe.
- **Settings screen:** settings are split into tabs (`SettingsTab`). New settings go into the tab they belong to.

## Layering: floating UI and the window preview

The live window preview on Home is a DWM thumbnail. DWM composites it above everything the app draws, so draw order can't put anything on top of it.

- **Floating surfaces:** anything drawn over the page goes between `ui_.BeginFloat()` and `ui_.EndFloat()`. That covers popovers, menus, toasts, tooltips and dialogs. `App::ApplyThumbnail` cuts the preview around every float.
  - `DrawFrame` already wraps the files list and the toast.
  - `Ui::End` wraps tooltips.
- **Native layers:** declare them with `ui_.NativeLayer(rect)` after drawing what lies under them. From then on, any drawing over them outside a float counts as a layering error:
  - `--screenshot` fails on it.
  - `--selftest` checks the files list, tooltip and toast cases.
- **Drawing directly on the device context** (`DrawBitmap`) bypasses the check. Call `ui_.Painted(rect)` for it.
- **Changes to the preview:** run `--preview-test <dir>`. It checks the real DWM output on screen.

## Tests and privacy

- **Window lists:** tests list and record only the "OzMaker Test Window" (`testWindowsOnly_`). Never save or show other windows' titles or contents.
- **Screen grabs:** capture only OzMaker's own window, kept topmost.
- **User settings:** never touch the user's `%APPDATA%\OzMaker\settings.ini` or the HKCU Run key from tests.
- **Microphone:** tests never open the real microphone (it records the room). `--selftest` checks the mixer on made-up streams, screenshots show a made-up device, and `--record-test ... mic` is for the user to run by hand.
