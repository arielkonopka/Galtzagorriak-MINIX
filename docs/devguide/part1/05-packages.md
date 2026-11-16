# 5. Packages, settings and installing into flash

Everything that is not in the base system arrives as a package. This chapter describes the
package format, the `pkg` tool, scripts that run at install, where an app keeps its settings and
data, and when to put program text into internal flash.

## 5.1 The package format

A package is a tar archive in the old POSIX ustar format, `name_version.tar`, made on the PC by
`flipper/tools/mkpkg.py`. It holds the files with their paths relative to `/`, and a `CONTROL`
directory:

| File | Contents |
|---|---|
| `CONTROL/control` | `Package:`, `Version:`, `Depends:` (comma or space separated names), `Description:`, `Abi:` (written by `mkpkg.py` from the programs' headers, or `any`), optionally `Os:` and `Flash: yes` |
| `CONTROL/postinst` | optional `sh` script, run after the files are installed |
| `CONTROL/prerm` | optional `sh` script, run before the files are removed |

Limits: a package name of at most 13 characters (MINIX file names are 14, and the transfer adds
nothing to that), and paths under 100 characters.

```
python3 -I flipper/tools/mkpkg.py -n tally -v 1.0 -o out \
    -d hello -D "Counts things" \
    --postinst tally.postinst \
    /usr/bin/tally=build/flipper/minix/bin/tally \
    /usr/lib/tally/defaults/tally.conf=tally.conf
```

File modes: 755 for files in a `bin` directory, 644 otherwise; `,MODE` after the source path
sets one explicitly (`,4755` for a set-uid program, which is then owned by root). Other files are
owned by `bin`. `Flash: yes` (install the programs into flash, as `--flash` does) is read by
`pkg` but `mkpkg.py` has no option for it yet; ask for one if you need it.

**ABI.** `elf2mnx.py` stamps each program with the system's ABI number (`GZ_ABI`,
`include/minix/gzversion.h`), and `mkpkg.py` copies it into `Abi:`. `pkg` refuses a package
built for another ABI, and `exec` refuses such a program with `ENOEXEC`. So a package built from
an older tree fails cleanly instead of crashing after an incompatible system update (spec J.3).

## 5.2 The `pkg` tool

| Command | What it does |
|---|---|
| `pkg get [--flash]` | receive packages over the console and install them (the PC side is `flsend`) |
| `pkg install [--flash] file.tar` | install a package file that is already on the Flipper |
| `pkg remove name` | run `prerm`, remove the files; refused if another package depends on it |
| `pkg list`, `pkg info name`, `pkg files name` | what is installed, a control file, a file list |
| `pkg flash file...`, `pkg unflash file...` | put a program's text into flash, or take it out |
| `pkg bind file...` | connect files to text already in flash (used by `/etc/rc` at boot) |
| `pkg budget [pages]` | show or set the run-time cache's daily erase budget |

The database is in `/usr/var/pkg/info` (`NAME.control`, `NAME.list`, and the scripts). `pkg`
needs `/usr` mounted, except for `bind` and `budget`. Dependencies are checked at install
(missing ones are named), not fetched: there is no repository yet.

## 5.3 Install scripts

`postinst` and `prerm` run as root with `/bin/sh`. Keep them short and **idempotent**: an
upgrade runs `postinst` again, and a card may be older or newer than you expect. The base
package's `flipper/rootfs/base.postinst` is the model; it checks before it changes anything:

```sh
# add the app to the launcher once
grep '^Tally:' /etc/apps >/dev/null || {
	echo 'Tally:nobody:/usr/bin/tally' >> /etc/apps
	echo "tally: added to the launcher"
}
# a device node that older cards lack
test -r /dev/ibutton || mknod /dev/ibutton c 6 9
```

Things to know:
- `test` here knows no `-c` or `-b`; `test -r` is the usual check for a device file.
- `/etc/apps` may be missing on the card once defaults live only in `/rom` (spec L.5); `>>` creates the card's
  copy with only your line, and the launcher then shows only that. Copy the default first if
  the card has none: `test -f /etc/apps || cfcat /etc/apps > /etc/apps`.
- `ed` is available for editing files in place; `sed -i` is not.
- Say what you changed on stdout; the user sees it on the console.

## 5.4 Settings: defaults in ROM, changes on the card

Settings are small text files in the stock `Key: value` style, read with
`FILE *cfopen(const char *path, const char *mode)` from `<stdio.h>` (spec L.3):

- **Reading** opens `path` on the SD card; if it is not there, `/rom` + `path`, the read-only
  defaults built into the firmware image.
- **Writing** always goes to the card, and makes the directories. Modes `a` and `+` start from a
  copy of the default if the card has none.
- **Deleting** the card's file brings the default back. An empty file is an empty setting.
- Only absolute paths fall back to `/rom`.

Shell scripts use `cfcat path` the same way. `settings diff` lists the files under `/etc` that
differ from their defaults; `settings reset [path...]` brings defaults back.

For a package's own defaults, ship them in the package (on the card), not in `/rom`, which only
the firmware image writes. The planned convention is `/usr/lib/<pkg>/defaults/` with the same
fallback (spec L.6, **(planned)**: `cfopen` does not look there yet, so read your default
explicitly when the user's file is missing). Per-app data goes to `/ext/apps_data/<app>/`
**(planned, F.2)**; until that directory exists, use `$HOME` or a directory your `postinst`
creates under `/usr/var`.

Never keep settings or data in internal flash: nothing a program writes at run time may land
there (spec L.1).

## 5.5 Program text in flash

Chapter 1 explained why: an installed program runs its text from flash and needs RAM only for
data and stack. How it works for a package author:

- `pkg install --flash` (or `flsend --flash`) writes each program's text into the **installed
  region** at install time, one page step at a time between other work, checks it, and records it
  by the file's (device, inode, ctime) and a checksum of its start. `exec` then finds it with no
  copy.
- A program that is not installed this way loads into RAM; after three runs MM copies it into
  the **run-time cache** in the background, within the daily erase budget.
- `free -v` shows both regions and the erase counters.
- An unchanged text is never written again: reinstalling the same program adds only a small
  record, and a new version writes its own pages once.

When to use which:

| Situation | Use |
|---|---|
| A release of an app people run every day | `--flash` |
| A test build you are iterating on | plain install, or run the file from `/tmp` |
| A tool run once in a while | plain install; the cache picks it up if it is run often |
| A program that is replaced daily by a script | plain install, and say why in its docs |

`free -v` shows how full the installed region is. It is compacted only when it runs out
of space, and a power cut at any point of an install leaves either the old or the new program,
never a broken one (spec AC2b.5, tested by `tools/powercut.py`).

## 5.6 Updating the system itself

The base system is the `base` package (`build/flipper/minix/pkgs/base_X.Y.tar`) plus the
firmware image. Its version (`BASE_VER` in `minix.mk`) goes up whenever its contents change.
Your package should not replace files of `base`; if it must change something the base system
owns (`/etc/rc`, `/etc/profile`), do it in `postinst` with a check, as `base.postinst` does.
