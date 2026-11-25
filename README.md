# Hidden Terminal

A full Linux terminal (GTK4 + VTE, the engine behind GNOME Terminal) that is
**left out of every screen capture**: screenshots, browser screen share, OBS,
Zoom, Discord, Teams. Captures show whatever is *behind* the window, with no
black box. You still see it normally on your monitor.

## How the hiding works
On Wayland the compositor produces every capture, so an app cannot hide itself,
and GNOME (Mutter) has no way to do it. Hiding is done by the **niri**
compositor's window rule (in `niri/hidden-terminal.kdl`):

```kdl
window-rule {
    match app-id=r#"^dev\.hidden\.Terminal$"#
    block-out-from "screen-capture" kind="hide"
}
```

`kind="hide"` is not in a niri release yet (it is newer than v26.04), so niri is built from git
commit `398026a8`. **The terminal is only hidden while you are logged into the
"Niri" session.** In GNOME it works as a normal terminal but will be captured.

Limits: niri's *interactive* screenshot UI (`Print`) still shows it, but the automatic
`screenshot-screen` / `screenshot-window` actions, third-party tools and all
portal screencasts do not. A camera pointed at the screen, root-level framebuffer
grabs (`ffmpeg kmsgrab`) or an HDMI capture card can still see it.

## Terminal features
bash (your `$SHELL`) or any program: vim, htop, tmux, ssh, mc. You also get
256 colours and truecolor, mouse, Unicode and emoji, 10k-line scrollback, tabs,
search, clickable URLs and OSC 8 hyperlinks (Ctrl+click), a right-click menu,
zoom and full screen.

| Keys | Action |
|---|---|
| Ctrl+Shift+T / N | new tab / new window |
| Ctrl+Shift+W / Q | close tab / close window |
| Ctrl+Shift+C / V (Ctrl+Insert / Shift+Insert) | copy / paste |
| Ctrl+Shift+A | select all |
| Ctrl+PgUp / PgDn, Alt+1..9 | switch tab |
| Ctrl+Shift+PgUp / PgDn | move tab |
| Ctrl+Shift+F, then Enter / Shift+Enter (or Ctrl+Shift+G / H) | find previous / next |
| Ctrl + / - / 0 | zoom |
| F11 | full screen |
| Ctrl+Shift+K | reset terminal |
| Shift+PgUp / PgDn | scroll |

CLI: `hidden-terminal [-w DIR] [-e "cmd args"] [-- cmd args...]`

## Build and install
```sh
sudo ./scripts/install.sh
```
This one command does everything: installs the apt dependencies and Rust (if
missing), builds the terminal, builds niri at commit `398026a8` and
xwayland-satellite `v0.8.3` in `~/src/`, installs them system-wide and writes
`~/.config/niri/config.kdl`. The first run takes several minutes (compiling niri).
It is safe to re-run.

To remove it all: `sudo ./scripts/uninstall.sh` (add `--packages` to also remove
the apt packages it installed).

`~/.config/niri/config.kdl` is niri's default config with waybar disabled and
`include ".../niri/hidden-terminal.kdl"` at the end. Then log out, click the gear
on the login screen, choose **Niri** and press **Super+T** (or Super+Return).

niri basics: Super+Shift+/ shows hotkeys, Super+D opens the launcher (fuzzel),
Super+Q closes a window, Super+Alt+L locks, Super+Shift+E logs out. GNOME is
untouched; pick it at login to go back.
