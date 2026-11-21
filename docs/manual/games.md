# Games

## Snake

**Snake** on the launcher (`@game` icon) is a small game: a snake moves on a grid, eats the `*`
fruit, and grows by one each time. The arrows turn it; running into a wall or into its own body
ends the game. As in the original, there is one "last chance" tick against a wall — turn away and
you survive. After a game over, **OK** starts a new game and **Back** leaves.

The board is drawn as text in a window (one character a cell: `@` the head, `o` the body, `*`
the fruit), so it fits the desktop's window apps like the others. The game loop itself is a small
library (`src/lib/snake`) with host unit tests (`flipper/tests/snake`); the app (`snakeui`) only
draws it and reads the keys.

Snake keeps no files and writes no flash; its whole state is a few kilobytes of RAM while it
runs.
