# Arch Linux packaging

`PKGBUILD` builds the Qt 6 / QtWebEngine GoldenDict from this repository.

```bash
cd packaging/arch
makepkg -si
```

The package is named `goldendict` and declares `conflicts`/`provides` for
`goldendict-git`, `goldendict-ng` and `goldendict-ng-git`, because all of them
install `/usr/bin/goldendict`. `pacman -U` therefore replaces an existing build
of the same program rather than keeping two side by side.

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
