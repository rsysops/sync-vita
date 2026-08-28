# VitaSync — PS Vita streaming & control over USB

Stream the PS Vita screen to your PC **and** control the console (sticks, buttons,
touch, triggers) over USB, all within a single Windows application.

Prototype based on a modified fork of **`vita-udcd-uvc` 1.7** (`xerpi/vita-udcd-uvc`, MIT license).

## Two components

- `VitaSync/` — Vita kernel plugin (`VitaSync.skprx`) : UVC + vendor HID USB
  interface. UVC device name: `VITASYNC`, USB PID `0x1337`.
- `windows/` — C#/.NET 8 WinForms application (`VitaSync.exe`): Media Foundation
  video, HID (HidSharp), keyboard/mouse mapping, touch, F8 FPS mode, built-in
  installation tutorial (5 pages, EN/FR/ES).

## Installing the plugin on the Vita

1. Copy `VitaSync.skprx` into `ur0:tai/` (VitaShell).
2. Add to `ur0:tai/config.txt`:

   ```
   *KERNEL
   ur0:tai/VitaSync.skprx
   ```

3. Reboot the Vita completely.

> The tutorial's download button fetches the embedded plugin automatically and
> copies it into your Downloads folder.

## Video troubleshooting

Sometimes the video does not start on the first try. In that case, restart the
video capture a few times (stop, then start again) until the image shows up.
Don't hesitate to insist: it is a known behavior of the prototype, the image
always eventually arrives.

## Building the Windows application

```powershell
cd windows
dotnet build -c Release
```

Executable: `windows\bin\Release\net8.0-windows10.0.22621.0\VitaSync.exe`
(`VitaPCControl.csproj` is the historical project name; the executable is `VitaSync.exe`).

User settings: `%APPDATA%\VitaSync\` (`sensitivity.txt`, `lang.txt`,
`keymap.txt`, `tutoDone.txt`). The tutorial only appears once (via `tutoDone.txt`).

## Misc

- `build` / `bin` folders intentionally ignored by Git (build artifacts).

## ⚠️ Warning

Prototype not tested as an official release. Test it on a real Vita before use.
The plugin loads in kernel — risk of hang (fallback: reboot the Vita while
holding **L** to skip plugins).