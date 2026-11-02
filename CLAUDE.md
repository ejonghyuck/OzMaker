# OzMaker

Win32 + Direct2D, immediate-mode UI (`src/Ui.*`). Views are in `src/AppViews.cpp`; test modes are in `src/SelfTest.cpp`. The README's "테스트용 명령줄" section lists them.

## Build and test

- Build: MSBuild `OzMaker.vcxproj /p:Configuration=Release /p:Platform=x64`. Without vcpkg you get a GIF/MP4-only build; WebP/AVIF are built in CI.
- CI runs `tools/ci-selftest.ps1`, which runs `--selftest`.
- After UI changes, also run `--screenshot <dir>` and look at the PNGs. It fails on layering errors.
- Point `OZMAKER_SETTINGS` at a scratch settings file for every test run.

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
