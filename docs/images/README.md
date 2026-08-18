# Screenshots and clips for the README

## Generated — do not edit by hand

| file | what it shows |
|---|---|
| `pipeline-segmentation.jpg` | a camera frame through `dart_seg_unet` to a masked, warped board, and why the mask flares past the steel |
| `why-three-cameras.jpg` | three warped views added together, meeting only at the tips; plus a dart occluded in one view |
| `architecture-detector.jpg` | the three camera streams, the funnel, and where the conditioning enters |
| `autoregressive-loop.jpg` | one real turn, one pass per throw, and why a frozen frame is different |

All four come from `scripts/make_model_figures.py`, which runs the shipped
ONNX models over one real labelled frame — every thumbnail is genuine model
output rather than an illustration. Re-run it after a model re-export. Edits
made directly to the images are overwritten, and until then they quietly
disagree with the model.

The script lays each figure out as SVG, rasterises it through headless Chrome
at 2× and downsamples to 1.4× before encoding JPEG. Supersampling is what keeps
the 9 px labels legible; going straight to 1.4× renders them mushy. They land around 160-340 KB each, against roughly 380 KB apiece as lossless
PNG — these canvases are mostly photograph, which is what PNG is worst at.

Thumbnails come from `scripts/make_figure_assets.py`, which needs a
DartModelTraining checkout for the frames, board transforms and tip labels:

```
python scripts/make_figure_assets.py --training-repo ../DartModelTraining
python scripts/make_model_figures.py
```

Assets land in `build/figassets/` by default; override with `$DARTMODELTRAINING`,
`--out`, or `$TREBLOTRON_FIGASSETS` for the second script. Neither script has any
path baked into it.

The loop figure is three captures of one real turn, found by nesting tips across frames
(the board drifts ~0.5 px between throws). Its conditioning is built by
differencing the segmenter between those frames, which is what the runtime does
— no per-dart label is used as model input anywhere in these figures.

## Captures — to be recorded

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
