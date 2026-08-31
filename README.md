# FloeWM: minimal floating window manager for X11

FloeWM is one C file (`floe.c`) using Xlib directly, no config file, no external dependencies beyond Xlib itself. Windows float where their client (or the user) places them — there is no tiling layout to compute or maintain, which is what keeps the whole program small enough to read start to finish in a few minutes.




## How it works

Under X11, a window manager is not a special, privileged component of the display server: it's a normal client that asks for one extra permission. FloeWM opens a connection to the X server and selects `SubstructureRedirectMask` on the root window. From that point on, the server no longer carries out other clients' requests to map, move, resize, or restack their windows directly — it hands each request to FloeWM as an event instead, and FloeWM decides what to do with it. Only one client can hold this privilege at a time, which is exactly what "being the window manager" means; FloeWM detects and refuses to start if another one already owns it.

The rest of the program is a single event loop (`main()`), reacting to whatever the server reports:

| Event                                            | What FloeWM does                                                                                        |
|--------------------------------------------------|---------------------------------------------------------------------------------------------------------|
| `MapRequest`                                     | A client wants to appear. FloeWM gives it a border and maps it as-is — floating means no repositioning. |
| `ConfigureRequest`                               | A client wants to move/resize/restack itself. FloeWM grants the request verbatim.                       |
| `EnterNotify`                                    | The pointer entered a window. FloeWM focuses it (focus-follows-mouse).                                  |
| `KeyPress`                                       | One of the three grabbed shortcuts (spawn terminal / close / quit).                                     |
| `ButtonPress` / `MotionNotify` / `ButtonRelease` | Alt+drag move/resize, driven by the pointer grab started on press.                                      |

There is no hidden state machine and no polling: if nothing happens on the X connection, FloeWM is asleep inside `XNextEvent()`. The only piece of state carried between events is which window is focused and, during a drag, which window/button/geometry started it.

Two X11 mechanics worth knowing about before reading the code:

- **Passive grabs** (`XGrabKey` / `XGrabButton` in `grab()`) are how FloeWM gets global keyboard/mouse shortcuts: it tells the server up front which key/button combinations to redirect to it instead of delivering them straight to the window under the pointer.
- **`WM_DELETE_WINDOW`** (used in `close_window()`) is the ICCCM convention for asking a client to close itself gracefully (so it can prompt for unsaved changes, etc.) instead of severing its X connection outright with `XKillClient`.




## Build

Requires a C compiler and the Xlib headers (e.g. `libx11-dev` / `libX11-devel`).

```
make
```

This produces a `floe` binary in the project directory.

Other targets:

```
make clean    # remove the built binary
make install  # install to $PREFIX/bin (default /usr/local), use DESTDIR/PREFIX to override
```




## Configure

There is no config file and no parser — FloeWM follows dwm's approach of config-as-source. All tunables live in the `config` block at the top of `floe.c`:

| Define         | Meaning                                         |
|----------------|-------------------------------------------------|
| `MOD`          | Modifier key for every binding (default: Alt)   |
| `BORDER_WIDTH` | Window border thickness in pixels               |
| `COL_FOCUS`    | Border color of the focused window (`0xRRGGBB`) |
| `COL_NORMAL`   | Border color of every other window              |
| `termcmd`      | argv used to spawn a terminal                   |

Edit the value you want, then `make` again — there's nothing to reload, just restart FloeWM.




## Use

Run `floe` in place of your usual window manager, or try it inside a nested X server first so it can't disturb your real session:

```
Xephyr -br -ac -screen 1280x800 :1 &
DISPLAY=:1 ./floe &
DISPLAY=:1 xterm &
```

To make it your actual session's window manager, launch it from your `.xinitrc`/`.xsession` (or, on a desktop like XFCE, disable the default WM's autostart and start `floe` instead — see your desktop environment's session settings).


### Keybindings

| Binding                 | Action                              |
|-------------------------|-------------------------------------|
| `Alt+Shift+Return`      | spawn a terminal                    |
| `Alt+Shift+c`           | close the focused window            |
| `Alt+Shift+q`           | quit FloeWM                         |
| `Alt` + drag (button 1) | move the window under the pointer   |
| `Alt` + drag (button 3) | resize the window under the pointer |

Focus follows the mouse: moving the pointer into a window focuses it.




## Known limitations (left out on purpose)

FloeWM optimizes for a small, readable core over feature completeness. Left out deliberately, in rough order of how likely they are to bite:

- **No NumLock/CapsLock-aware key grabs.** Shortcuts are grabbed for the exact `Mod1|Shift` combination, so they won't fire while NumLock (or another lock modifier) is active. Fixing this means re-grabbing every binding once per lock-modifier combination (dwm does this in about 15 lines).
- **Assumes a TrueColor default visual.** Border colors are set as raw `0xRRGGBB` pixel values, which only works directly on a TrueColor visual. Fine on virtually every modern setup, but not portable to a palette-based visual.
- **No EWMH (`_NET_*`) support.** FloeWM doesn't announce itself or a window list via the EWMH properties, so external panels, task bars, and tools like `wmctrl` won't see or control it.




## TODO

- [ ] NumLock/CapsLock-aware key grabs, so shortcuts fire regardless of lock-modifier state.
- [ ] Basic EWMH (`_NET_*`) support, so panels, task bars, and `wmctrl` can see and interact with FloeWM.
- [ ] Recognize windows that shouldn't be treated like ordinary top-level windows — dialogs/popups via `_NET_WM_WINDOW_TYPE`, `WM_NORMAL_HINTS` size hints, and override-redirect windows (menus, tooltips) that must be left untouched.
- [ ] Refactor the core around a small set of internal commands (focus, close, spawn, move, resize, quit) exposed over a socket, bspwm-style, so the WM itself stays a mute core driven by an external client.
- [ ] Once the socket/command layer exists, a small Guile client to script configuration and behavior from outside the WM, without embedding an interpreter into the core.
