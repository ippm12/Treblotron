# Screenshots and clips for the README

Drop captures here and uncomment the corresponding block in the root README.

Worth having, in order of value:

| file | what it should show |
|---|---|
| `hero.gif` | Dartfleet mid-game — darts landing, ships sinking. The single strongest asset the page can have. |
| `calibration.png` | The calibration screen with three live previews and some points placed. Shows setup is real work but finite. |
| `scoring.gif` | A dart landing and the score updating, ideally with the board visible. Proves the claim on the tin. |
| `vision-debug.png` | The heatmap overlay. For the "how it works" section. |

Keep GIFs under about 5 MB — GitHub renders them inline and a large one makes
the page crawl. `ffmpeg -i clip.mp4 -vf "fps=12,scale=720:-1" -loop 0 out.gif`
is a reasonable starting point.
