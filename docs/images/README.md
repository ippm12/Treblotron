# Screenshots and clips for the README

## Generated — do not edit by hand

| file | what it shows |
|---|---|
| `pipeline-segmentation.jpg` | a camera frame through `dart_seg_unet` to a masked, warped board |
| `tip-flare.jpg` | four 5× tip crops: the mask ends in a rounded cap wider than the steel |
| `why-three-cameras.jpg` | three warped views added together, meeting only at the tip |
| `occlusion.jpg` | one dart cut in two by an earlier one in one view, whole in another |
| `architecture-detector.jpg` | the three camera streams, the funnel, and where the conditioning enters |
| `autoregressive-loop.jpg` | one real turn, one pass per throw |
| `frozen-frame.jpg` | why replaying a still frame stops after one dart |

All seven come from `scripts/make_model_figures.py`, which runs the shipped
ONNX models over real labelled frames — every thumbnail is genuine model output
rather than an illustration. Re-run it after a model re-export. Edits made
directly to the images are overwritten, and until then they quietly disagree
with the model.

**They carry labels, not prose.** A caption, a tensor shape or a readout only
means anything next to the thing it points at, so it stays in the image;
explanation reflows and belongs in the README. That is not only tidiness — a
figure 1400 px wide with 10 px prose renders at about 350 px on a phone, which
turns the text into two and a half pixels of grey. It also let several figures
lose a third of their width, so what remains renders larger. Some are split for
the same reason: two arguments in one image force the README to say both before
showing either.

The script lays each figure out as SVG, rasterises it through headless Chrome
at 2× and downsamples to 1.4× before encoding JPEG. Supersampling is what keeps
the 9 px labels legible; going straight to 1.4× renders them mushy.

Thumbnails come from `scripts/make_figure_assets.py`, which needs a
DartModelTraining checkout for the frames, board transforms and tip labels:

```
python scripts/make_figure_assets.py --training-repo ../DartModelTraining
python scripts/make_model_figures.py
```

Assets land in `build/figassets/` by default; override with `$DARTMODELTRAINING`,
`--out`, or `$TREBLOTRON_FIGASSETS` for the second script. Neither script has any
path baked into it.

The loop figure is three captures of one real turn, found by nesting tips across
frames (the board drifts ~0.5 px between throws). Its conditioning is built by
differencing the segmenter between those frames, which is what the runtime does
— no per-dart label is used as model input anywhere in these figures.

## Captures

Photographs and screenshots, kept as they came off the camera and the app --
nothing here is generated, so re-running the figure scripts will not touch them.

| file | what it shows | used in |
|---|---|---|
| `live-x01.jpg` | the rig and the board, three darts in, X01 showing the scores they made | README hero |
| `live-dartfleet.jpg` | the same rig running Dartfleet | README hero |
| `live-cricket.jpg` | the same rig running Cricket | README hero |
| `game-dartfleet.png` | Dartfleet full screen: two boards, ships, hit markers | The games |
| `game-x01.png` | X01 full screen: running score, leg history, four-player board | The games |
| `game-cricket.png` | Cricket full screen: marks grid for three teams | The games |

The live shots are 480 x 640. That is enough at the size the README renders
them, three to a row, but too small to enlarge -- if the originals off the phone
are still around they would be worth keeping instead.

## Still worth having

| file | what it should show |
|---|---|
| `calibration.png` | The calibration screen with three live previews and some points placed. Setup is the one real cost, and showing it makes it finite rather than ominous. |
| `hero.gif` | Dartfleet mid-game -- darts landing, ships sinking. The only thing a still cannot do is show a dart being scored as it lands. |
| `vision-debug.png` | The heatmap overlay, for "How it works" -- the one image that would show the model running live rather than reconstructed offline. |

Keep GIFs under about 5 MB -- GitHub renders them inline and a large one makes
the page crawl. `ffmpeg -i clip.mp4 -vf "fps=12,scale=720:-1" -loop 0 out.gif`
is a reasonable starting point.
