# gtk2-portal-chooser

Native file dialogs for **3DCoat on Linux** (and other GTK2 apps).

3DCoat's Linux build uses the old GTK2 file chooser: no thumbnails, no bookmarks from your
file manager, and it looks out of place on a modern desktop. GTK2 has no support for
[xdg-desktop-portal](https://flatpak.github.io/xdg-desktop-portal/), so there is no setting
or environment variable that changes it.

This is a small `LD_PRELOAD` library that intercepts GTK2's file chooser and opens your
desktop's file chooser through the portal instead: GNOME's or KDE's dialog, or a file
manager that implements the portal's FileChooser (I use [Strata](https://github.com/lgse/strata)).
The app doesn't notice: it gets the chosen path back exactly as from the GTK2 dialog.

## What works

* **Open**, **Save** (the suggested file name and folder are filled in), multiple selection
  and folder pickers.
* **File type filters.** 3DCoat uses GTK2 *custom* filters (a callback, no patterns), which
  the portal can't take. The library probes each callback with a list of known extensions
  (3DCoat formats, meshes, images, …) and turns the result into a *Supported (obj, fbx, …)*
  filter plus *All files*. Patterns are added in lower and upper case, because portal file
  choosers match case-sensitively.
* **Fallback:** if the portal call fails (no portal running, no FileChooser backend), the
  normal GTK2 dialog opens.

Not supported: extra widgets an app puts inside its GTK2 dialog (`gtk_file_chooser_set_extra_widget`)
don't appear in the portal dialog.

## Status

Tested on Ubuntu with Hyprland, xdg-desktop-portal 1.21 and Strata as the FileChooser
backend, with the included `test_chooser` program (which builds its dialog the way 3DCoat
does), on the host and inside the 3DCoat 2026.25 Flatpak sandbox. Reports from other
desktops and 3DCoat versions are welcome: open an issue.

## Build

Needs a C compiler, `pkg-config` and the GLib/GIO development files. GTK2 headers are **not**
needed.

```bash
# Debian/Ubuntu: sudo apt install build-essential pkg-config libglib2.0-dev
# Arch:          sudo pacman -S base-devel glib2
# Fedora:        sudo dnf install gcc make pkgconf glib2-devel
git clone https://github.com/SreckoM/gtk2-portal-chooser.git ~/src/gtk2-portal-chooser
cd ~/src/gtk2-portal-chooser
make
```

Test it (opens your portal file chooser, prints the chosen file; needs the `libgtk2.0-0`
runtime library):

```bash
make test
```

## Use with 3DCoat

### Flatpak (Pilgway's `com.pilgway.coat`)

One-time override, so every way of starting 3DCoat picks it up:

```bash
flatpak override --user --env=LD_PRELOAD=$HOME/src/gtk2-portal-chooser/gtk2_portal_chooser.so com.pilgway.coat
```

This needs the `.so` to be visible inside the sandbox. Pilgway's Flatpak has
`filesystems=host`, so a path under your home works. Check with
`flatpak info --show-permissions com.pilgway.coat`.

Undo: `flatpak override --user --unset-env=LD_PRELOAD com.pilgway.coat`

### Unpacked / tarball build

Start 3DCoat with the library preloaded, e.g. in your launcher script:

```bash
export LD_PRELOAD="$HOME/src/gtk2-portal-chooser/gtk2_portal_chooser.so${LD_PRELOAD:+:$LD_PRELOAD}"
exec ./3dcoat "$@"
```

### Other GTK2 apps

The same `LD_PRELOAD` works for any GTK2 app that uses `gtk_file_chooser_dialog_new` +
`gtk_dialog_run`. Apps that drive the dialog with signals instead of `gtk_dialog_run` are
not handled.

## Which dialog you get

That's decided by xdg-desktop-portal, not by this library. To pick a backend for the
FileChooser, e.g. in `~/.config/xdg-desktop-portal/portals.conf` (or `<desktop>-portals.conf`):

```ini
[preferred]
default=gtk
org.freedesktop.impl.portal.FileChooser=gnome;gtk;
```

## Environment variables

| Variable | Effect |
|---|---|
| `GTK2_PORTAL_DEBUG=1` | Log what the library does to stderr |
| `GTK2_PORTAL_DISABLE=1` | Use the normal GTK2 dialog |

## Troubleshooting

* **Still the GTK2 dialog:** run with `GTK2_PORTAL_DEBUG=1` from a terminal. No `[gtk2-portal]`
  lines → the library isn't loaded (check the path, and for Flatpak that the sandbox can see it).
  `portal call failed` → no portal FileChooser is running: `busctl --user list | grep portal`.
* **A format is missing from the filter:** add its extension to `probe_exts` in
  `gtk2_portal_chooser.c` and run `make` again, or pick *All files*.

## License

MIT
