# SaidaEngine 1.0.0 Beta 4 — Linux x86_64

This archive is the self-contained, per-user Linux distribution of the
SaidaEngine editor. It includes the editor, Hub, game runtime template, CLI,
compiled shaders, editor assets, fonts, sample project and the non-system ELF
libraries used by the build.

## Supported systems

- x86_64 Linux with glibc 2.35 or newer (Ubuntu 22.04/24.04, Debian 12 and
  compatible distributions);
- an X11 or XWayland graphical session;
- a GPU and installed Intel, AMD or NVIDIA graphics driver exposing Vulkan 1.3.

The Vulkan **SDK**, compiler, CMake, GLFW development package and other build
tools are not required. The graphics driver is not bundled: it is specific to
the machine and must be installed normally by the distribution or GPU vendor.
The release is compiled for the baseline x86-64/SSE2 instruction set; AVX and
AVX2 are not required.

## Install for the current user

```bash
tar -xzf SaidaEngine-v1.0.0-beta.4-linux-x86_64.tar.gz
cd SaidaEngine-v1.0.0-beta.4-linux-x86_64
./install.sh
```

No `sudo` is used. The payload is installed under
`${XDG_DATA_HOME:-$HOME/.local/share}/SaidaEngine/1.0.0-beta.4`, launchers are
placed under `${XDG_BIN_HOME:-$HOME/.local/bin}`, and a desktop-menu entry is
created under `${XDG_DATA_HOME:-$HOME/.local/share}/applications`.

If `$HOME/.local/bin` is not in `PATH`, log out and back in or add it to your
shell configuration. Then launch:

```bash
saidaengine-hub
```

The editor can also be started directly with `saidaengine-editor`, and the CLI
with `saidaengine-tool`.

## Portable use without installation

From the extracted directory:

```bash
./payload/SaidaEngineHub
```

Keep the complete `payload` directory together. Moving only an executable will
separate it from its private libraries, shaders and assets.

## Build and run a Linux game

Open the Build window in the Linux editor, select **Linux (Vulkan)**, choose a
startup scene and use **Build** or **Build & Run**. The exported directory
contains its own native runtime libraries. The same operation is available from
the installed CLI:

```bash
saidaengine-tool export-game /path/to/Game.saidaproj \
  --platform linux --out /path/to/output
```

## Uninstall

```bash
"${XDG_DATA_HOME:-$HOME/.local/share}/SaidaEngine/1.0.0-beta.4/uninstall.sh"
```

Projects and user state under `~/.local/share/saidaengine` are deliberately not
deleted.

## Integrity and troubleshooting

`install.sh` verifies every payload file against `PAYLOAD-SHA256SUMS.txt` before
copying it. The release also publishes `SHA256SUMS-LINUX.txt` for the archive.

If startup reports that Vulkan 1.3 is unavailable, update or install the normal
graphics driver for the machine. On a server or container without a graphical
session, the editor and Hub are not supported; `saidaengine-tool` remains usable
for headless commands.
