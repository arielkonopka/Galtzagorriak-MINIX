# Buttons and gestures

**(planned)** (spec R6.10, task T16). This chapter describes button gestures that work in every
app. Until T16 is merged they do nothing.

## Backlight gestures

Two button chords control the backlight from anywhere, in the launcher and inside apps:

| Hold for 1.5 s | Effect | Hold again |
|---|---|---|
| **Left + Up** | Backlight **forced off**: it stays dark, even when you press keys | back to normal |
| **Right + Down** | Backlight **forced on**: it never goes off by itself | back to normal |

"Normal" is the usual idle timeout: the light goes off 15 seconds after the last key, and the
next key turns it on again. Holding the other chord switches straight from one forced mode to the
other.

- Press both buttons (in any order) and keep holding. The status LED blinks red, faster and
  faster; when it stops, a short buzz confirms the change.
- Let go before the buzz and nothing changes. Pressing a third button also cancels.
- **Back is never part of a gesture**, so holding Back still parks or closes the app as usual.
- In forced-off mode your key presses still reach the app, so you can keep using it in the dark.
  On battery the screen itself still goes blank 2 minutes after the last key (see
  [Power and battery](power.md)); the key that brings it back only wakes it.
- The app also sees the two buttons as normal presses. Games that need these combinations can
  switch gestures off while they run (`Gestures: no` in their launcher entry).
- A restart always brings the normal backlight back.

From the shell (as root):

```
backlight              # brightness, timeout, and the mode: auto, forced off or forced on
backlight force off    # the same as the Left + Up gesture; also: force on, force auto
```

The mode is kept in a register of the real-time clock, not in flash, so changing it never wears
the flash.

## Your own gestures **(planned)**

A **Gestures** app (spec R7.2) will let you change the chords, the hold time (0.5 to 5 s) and
the actions, and add gestures that start a program (for example a TV-remote "mute" or the mouse
jiggler). Its settings are stored in `/etc/flipper/gestures.conf` on the SD card, not in the
Flipper's flash; without the SD card the two gestures above apply.
