# kui: the app toolkit

`src/kui/`, with app sources in `src/apps/`. kui is how Kurono apps are moving out of the
kernel and into ring 3.

## The idea

One app source, two backends:

- `KUI_BACKEND_KERNEL` (default): the app is drawn by the in-kernel window manager. This
  is the fallback.
- `KUI_BACKEND_WAYLAND`: the app is a musl-static ring-3 program that draws into a
  `wl_shm` buffer and shows up through Kurono's own Wayland server. It gets the same
  title bar, motion, dock and focus as everything else.

The API (`kui.h`) has windows, a canvas, theme, events, timers, a filesystem wrapper,
a clipboard (with primary selection), and audio. Rules for app code: no exceptions, no
RTTI, no std library, no heap in the draw path. Version 1 is frozen; later versions only
add things.

## Which apps are ring 3

These build as ring-3 programs: calculator, text editor, files, task manager, media
player, video player (denji), terminal, and settings. The kernel launcher
(`kui_launch.cpp`) holds the table of apps. To run them as ring-3 programs, boot with:

```
kurono.r3apps=all
kurono.r3apps=calculator,texteditor,files,taskmgr
```

The boot value is cut off at 47 characters, so use `all` or a short list. A longer list
can go in the `apps.ring3` key of `ui.conf` instead. An app not in the list runs the in-kernel version of the
same source. You can also launch one from the shell with `kapp <name>`.

## Where the pieces are

- `src/kui/kui.h` the API
- `src/kui/kui_kernel.cpp` the kernel backend
- `src/kui/ring3/kui_wayland.cpp` the ring-3 backend (Wayland client)
- `src/kui/ring3/kui_r3_audio.cpp` ring-3 audio. A small PulseAudio native-protocol
  client talking to Kurono's pulse-compatible server
- `src/kui/ring3/kui_r3_text.cpp` text/clipboard bits
- `src/kui/kui_launch.cpp` app registry and launcher (`/system/apps/<name>`)
- per-app Makefile rules in `src/Makefile` (`KUI_*` variables) build the ELFs and embed
  them in the kernel

## Settings is a service

Settings pages used to read and write kernel globals directly, which can't work from
ring 3. Now the kernel runs a **settings service** on a unix socket
(`/system/run/user/1000/settings-svc`) and the ring-3 Settings talks to it. Reads come
as snapshots of config and hardware info. Writes go through a whitelist: every key has a
type and a range, and unknown keys or bad values are rejected. Privileged actions (adding
a user, starting the Linux layer, display changes, and so on) call the admin gate before
anything happens. The page code is the same source as the kernel build, compiled for the
Wayland backend.

## Task Manager

The ring-3 Task Manager is a normal program that reads `/proc` like any Linux tool. Each
task (user or kernel) has a numeric directory with `status`, `stat`, and threads under
`task/<tid>`. Per-task heap numbers appear when `kurono.taskmem=1`.

## Text editor

`src/apps/editor/`. Tabs, a file sidebar, syntax highlighting, regex find and replace,
multi-level undo, and X11-style primary selection (select to copy, middle click to
paste).

## Writing a new kui app

Look at `src/apps/calculator.cpp` first. It's the smallest one. Then copy its Makefile
rule. The general (older) `.kro` app format is described in `apps/KURONO_APP_DEV_GUIDE.md`.
