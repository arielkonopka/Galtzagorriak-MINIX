# Files on the Flipper: the Archive app

**Archive** on the launcher is the stock firmware's file browser for the files under
`/home/flipper` (spec Phase 7). It is a window app (the launcher draws it, see
[The launcher](launcher.md)), so it looks like the other apps and takes little memory.

## Tabs

| Tab | Shows |
|---|---|
| Favourites | the files you marked, from any tab |
| Infrared | `/home/flipper/infrared/*.ir` |
| Sub-GHz | `/home/flipper/subghz/*.sub` |
| 125 kHz RFID | `/home/flipper/lfrfid/*.rfid` |
| NFC | `/home/flipper/nfc/*.nfc` |
| iButton | `/home/flipper/ibutton/*.ibtn` |
| Text | `/home/flipper/*.txt` |
| Browser | everything in `/home/flipper`; folders end in `>`, OK goes into them |

Long lists come a page (8 rows) at a time. The app reads only the page on the screen from the
folder, so a folder with hundreds of files uses no more memory than one with ten.

## A file's actions

OK on a file opens its page:

- **Open**: a `.txt` file shows its first lines. Opening other types in their app from here is
  **(planned)**, the next part of issue #389. Until then the page names the app to use
  (Infrared, RFID, `subghz`, `ikey`).
- **Name**: rename it on the keyboard. The type ending stays: `telebista` becomes
  `telebista.ir`. A name that is already taken is not used.
- **Favourite**: on puts the file in Favourites; off takes it out.
- **Delete**: asks first, then removes the file (and its favourite).

## Favourites file

`/home/flipper/favorites.txt` holds the favourites, one full path per line, like stock's
`favorites.txt`:

```
/home/flipper/infrared/Egongela_telebista.ir
/home/flipper/subghz/Garajeko_atea.sub
```

You can edit it with `mined`. A path whose file is gone stays in the list until you remove it.
