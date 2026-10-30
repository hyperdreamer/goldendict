# Arch Linux packaging

`PKGBUILD` builds the Qt 6 / QtWebEngine GoldenDict from this repository.

```bash
cd packaging/arch
PKGDEST="$PWD/dist" SRCDEST=/var/tmp/makepkg-src BUILDDIR=/var/tmp/makepkg-build makepkg -si
```

`PKGDEST` puts the built package in `packaging/arch/dist/` (git-ignored) instead of the
current directory. `SRCDEST` and `BUILDDIR` must stay **outside** the repository:
makepkg keeps a bare clone there and creates its working copy from it, and git refuses
that operation when the bare repository lives inside another work tree
(`fatal: this operation must be run in a work tree`). Neither variable points at `$HOME`.

To install a package that was already built:

```bash
sudo pacman -U packaging/arch/dist/goldendict-*.pkg.tar.zst
```

The package does not build detached debug symbols (`options=('!debug')`); add the flag
back in `PKGBUILD` if you need them for a crash report.

## Why the build happens in the source tree

`ExternalViewer` is not the only place this matters: `mainwindow.cc` and
`portalhotkeys.cc` each declare a `Q_OBJECT` class inside the `.cc` file and end
with `#include "<name>.moc"`. qmake 6.12 emits the object's dependency on that
generated file but not the rule that produces it when the build directory is
outside the source tree, so the build fails with
`fatal error: mainwindow.moc: No such file or directory`. Building inside the
source tree — which is what `makepkg` does in `$srcdir` — works.

## Optional features

The qmake project detects optional dependencies at configure time and drops the
feature when the headers or libraries are missing:

| feature | configure switch | packages |
| --- | --- | --- |
| KDE window activation | `HAVE_KWINDOWSYSTEM` (header **and** library) | `kwindowsystem` |
| Zim support | `CONFIG+=zim_support` | `xz`, `zstd` |
| EPWING | `CONFIG+=no_epwing_support` disables it | `libeb` |
| Chinese conversion | `CONFIG+=no_chinese_conversion_support` disables it | `opencc` |
| legacy Hunspell API | `CONFIG+=old_hunspell` | `hunspell` |

Add the corresponding package to `depends` (or `optdepends`) if you enable a
feature that is not in the default set above.
