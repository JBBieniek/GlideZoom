# GlideZoom

A standalone screen magnifier for Windows 10/11 (native x64), with smooth zoom and support for multiple monitors. PowerToys is not required.

## Features

- Zoom each monitor independently, select one monitor, or link the whole desktop.
- Adjust zoom speed, wheel acceleration, pointer speed, and automatic pointer compensation for zoom.
- Show an optional overview preview, an outline showing the viewport's position on the full screen, or both, with adjustable position, opacity, and fading.
- Fit a resizable window to the magnified viewport and copy the magnified view to the clipboard.

## Build and run

Requires Visual Studio 2022 or 2026 with **Desktop development with C++**, a Windows SDK, and compatible CMake tools.

```powershell
.\Build.cmd
```

This builds the application and installer and runs the tests. Run the resulting **GlideZoom-Setup.exe**, then choose **Open GlideZoom** after installation.

This is a development build. Setup asks for administrator approval and explicit permission to trust a locally generated signing certificate. Use the installer to prepare the app for Windows UIAccess.

Open the app from its tray icon to change settings. **Advanced settings** contains the preview, outline, pointer, and acceleration options. Preview indicators are off by default.

## Controls

| Shortcut | Action |
| --- | --- |
| Left Ctrl + Left Alt + mouse wheel | Zoom in or out; the wheel modifier is configurable |
| Ctrl + Left Alt + Z | Toggle zoom on the current monitor |
| Ctrl + Left Alt + Shift + Z | Toggle linked desktop zoom |
| Ctrl + Left Alt + I | Toggle independent monitor zoom |
| Ctrl + Left Alt + V | Reveal the viewport indicator when enabled |
| Ctrl + Left Alt + Shift + F | Fit the active resizable window to the viewport |
| Ctrl + Left Alt + S | Copy the magnified view; capture briefly returns to 1× |
| Ctrl + Alt + Esc | Reset all zoom to 1× |

Use **Left Alt** for zoom shortcuts; Right Alt / AltGr remains available for typing.

The PowerToys integration lives in the [PowerToys fork](https://github.com/JBBieniek/PowerToys/tree/codex/glidezoom).

## License

[MIT](LICENSE).
