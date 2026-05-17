# pacdot

`pacdot` backs up and restores a personal Arch Linux setup. It is designed around a simple export tree and a TOML config file, with support for dotfiles, miscellaneous files, custom commands, systemd enabled units, and package lists.

## Features

- Back up and restore dotfiles.
- Back up and restore arbitrary files and directories, such as `~/Pictures/Wallpapers`.
- Run custom commands during export or restore.
- Back up and restore systemd enabled units.
- Back up and restore package lists.
- Restore protected system paths with elevated permissions when needed.
- Restore Flatpak packages with user-repo initialization support.
- Show the resolved config and export paths with `pacdot paths`.

## Supported Package Managers

`pacdot` does not manage every package manager directly. It restores package lists for:

- `pacman` for official Arch packages.
- `paru` for AUR packages.
- `flatpak` for Flatpak applications.

It also restores enabled `systemd` and `systemd --user` units.

## Usage

Build the project, then run one of the commands below.

```bash
pacdot paths
pacdot export [--clean-first]
pacdot restore [--dry-run] [--install-packages]
pacdot dotfiles export [--clean-first]
pacdot dotfiles restore [--dry-run]
pacdot files export [--clean-first]
pacdot files restore [--dry-run]
pacdot packages export [--clean-first]
pacdot packages restore [--dry-run]
```

`pacdot paths` prints the resolved locations used by the program:

- config: `~/.config/pacdot/pacdot.toml`
- export root: `~/.local/share/pacdot/export/`

`export` writes the current state into the export root.

`--clean-first` clears the export root before exporting.

`restore` applies the exported state back to the machine. Use `--install-packages` to restore package lists in addition to dotfiles, files, and systemd units.

`--dry-run` prints the actions without changing the system.

## `pacdot.toml`

`pacdot.toml` lives at `~/.config/pacdot/pacdot.toml`.

example:

```toml
[dotfiles.DankMaterialShell]
paths = [
    "~/.config/DankMaterialShell",
    "~/.local/state/DankMaterialShell"
]
backup_empty_files = false
backup_empty_dirs = false

[files.wallpapers]
paths = [
    "~/Pictures/Wallpapers",
]

[commands]
restore = [
    ["dms", "greeter", "enable"],
    ["dms", "greeter", "sync"],
]

[systemd]
backup_enabled_units = true
backup_user_enabled_units = true

[packages]
backup = true
exclude_pacman = [
    "steam",
]
exclude_aur = [
    "paru-debug",
]
exclude_flatpak = [
    "com.jetbrains.CLion",
]
```

### Config Sections

#### `dotfiles`

Use this for configuration files and directories under your home directory.

Each section name becomes the backup group name.

Supported keys:

- `paths`: one or more source paths.
- `exclude` or `excludes`: paths or glob patterns to skip.
- `backup_empty_files`: defaults to `true`.
- `backup_empty_dirs`: defaults to `true`.

Example:

```toml
[dotfiles.starship]
paths = [
    "~/.config/starship.toml",
]
```

#### `files`

Use this for miscellaneous data you want to back up separately from dotfiles.

The syntax is the same as `dotfiles`.

Example:

```toml
[files.wallpapers]
paths = [
    "~/Pictures/Wallpapers",
]
```

#### `commands`

Use this to run extra commands during `export` or `restore`.

Each command is written as an array of strings. The first item is the executable, and the rest are arguments.

Supported keys:

- `export`: commands to run after a successful `pacdot export`.
- `restore`: commands to run after a successful `pacdot restore`.

Example:

```toml
[commands]
restore = [
    ["dms", "greeter", "enable"],
    ["dms", "greeter", "sync"],
]
```

#### `systemd`

Supported keys:

- `backup_enabled_units`: back up system enabled units.
- `backup_user_enabled_units`: back up user enabled units.

#### `packages`

Supported keys:

- `backup`: enable package list export and restore.
- `exclude_pacman`: names to skip from the pacman list.
- `exclude_aur`: names to skip from the AUR list.
- `exclude_flatpak`: application IDs to skip from the Flatpak list.

## Installation
#### AUR:
https://aur.archlinux.org/packages/pacdot-bin

## Build

Requirements:

- A C++23 compiler
- CMake 3.20 or newer

Build with:

```bash
cmake -S . -B build
cmake --build build
```

The resulting binary is written to `build/pacdot`.

## Third-Party Libraries

`pacdot` includes these third-party libraries in the repository:

- `glob-cpp` for glob pattern matching.
- `tomlplusplus` for TOML parsing.

Both are vendored under `Dependencies/`.
