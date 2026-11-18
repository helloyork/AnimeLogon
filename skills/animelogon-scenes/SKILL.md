---
name: animelogon-scenes
description: Write, fix, check and package AnimeLogon scenes, the animated lock-screen wallpapers described by a scene.xml (root element <scene format="2">) with its pictures, masks, frame sequences, particles, effects, Lottie animations and HLSL shaders, packed as an .alscene. Use when asked to create or change an AnimeLogon scene or animated wallpaper, bring an illustration to life (swaying hair, breathing, blinking, falling petals, glowing lights), write a scene layer or effect shader, or find out why AnimeLogon refuses a scene.
license: MIT. See LICENSE in this folder.
---

# AnimeLogon scenes

A scene is an animated wallpaper: one XML file, `scene.xml`, and the files it names. It declares
nodes drawn in order (pictures, fills, particle emitters, groups), motions that move their
properties, effects that bend, light and colour them, a camera that gives depth, and settings the
user can change in the AnimeLogon app. Every frame is a function of the moment and the inputs
(clock, battery, what is playing, the sound, waking), so any moment can be drawn directly. Nothing
in it runs but its shaders, which keep to a fixed contract.

Scenes are not yet offered by released versions of the AnimeLogon app; the format and tools here
are what development builds read.

Read these as you need them:

- [reference/scene-format.md](reference/scene-format.md): the package, the stage, assets, nodes and
  placing them, the camera, motions, effects, particles, data, settings, the shader contract, and
  what makes a scene fast. Read it before writing a scene from scratch.
- [reference/attributes.md](reference/attributes.md): every attribute of every node, particle
  emitter and built-in effect with its values, default and meaning, the particle presets, the data
  names and the limits. Generated from AnimeLogon's reader.
- [examples/](examples/): complete scenes that AnimeLogon accepts.

## Workflow

1. **Pin down the brief.** What the picture is, what should move and how much, what the user should
   be able to change. For an illustration, find out which parts move (hair, clothes, eyes, lights,
   weather) and get the picture in layers: a background, the character, and the moving parts on
   their own layers or with grey masks painted over them. Ask for the layers or masks you need
   rather than guessing at a flat picture.
2. **Start from something close.** Copy the nearest example below.
3. **Write `scene.xml`** by the rules below. Check each attribute's values in `attributes.md`.
4. **Check it** with `scene_check` (next sections) and fix every error. Errors name the line. Read
   the warnings: they point at what will look wrong or run slowly.
5. **Look at it** with `scene_preview`: the still moment, other moments (`--time`), a few frames
   (`--frames 8 --fps 4`), other screen shapes (`--width 2560 --height 1080`) and other values of
   the settings (`id=value`).
6. **Deliver it** as an `.alscene` packed with `scene_check --pack`.

| Example | Shows |
|---|---|
| `examples/night-sky/` | A gradient the user recolours, twinkling stars they can thin or turn off, a drifting camera. |
| `examples/snowfall/` | Snow at two depths, a choice of how heavily it falls, a ground, film grain. |
| `examples/aurora/` | A layer shader fed by colour settings, a `drive` from the music, stars. |
| `examples/ripple/` | An effect shader reading the node's picture, with a setting in a parameter slot. |

The examples draw only fills, particles and shaders so they carry no pictures; real scenes are
usually built from pictures.

## Skeleton

```xml
<?xml version="1.0" encoding="utf-8"?>
<scene format="2" name="Garden" author="Your name" width="1920" height="1080" still="3">
  <settings>
    <toggle id="petals" label="Falling petals" default="on"/>
    <number id="sway" label="Hair sway" min="0" max="1" step="0.05" default="0.5"/>
  </settings>
  <assets>
    <image id="sky" file="art/sky.png"/>
    <image id="girl" file="art/girl.png"/>
    <mask id="hair" file="art/hair-mask.png"/>
    <frames id="eyes"><frame file="art/eyes-open.png"/><frame file="art/eyes-half.png"/><frame file="art/eyes-shut.png"/></frames>
    <image id="petal" file="art/petal.png"/>
  </assets>
  <camera>
    <wander property="x" amplitude="16" period="40"/>
  </camera>
  <layer source="sky" full="true" depth="0.1"/>
  <group x="1200" y="1080" anchor="bottom" height="980" depth="0.6">
    <layer source="girl">
      <effect type="bend" mask="hair" strength="$sway" period="3.4" root="top"/>
    </layer>
    <layer source="eyes" x="236" y="214" fps="24" repeat="bounce" gap="2.5-6"/>
  </group>
  <particles preset="petals" source="petal" when="$petals" depth="0.9"/>
</scene>
```

## Rules the reader enforces

Most refusals come from these. The full rules are in `scene-format.md`.

- Root `<scene format="2" name="...">`; `name` is required. `<settings>`, `<assets>` and `<camera>`
  come at most once each; nodes follow in drawing order.
- Unknown elements and attributes are errors, not ignored. Each node takes only its own attributes
  and the common ones (`attributes.md`); effects take only their own.
- Every asset has a unique id and one `file` (a `<frames>` has 1 to 120 `<frame file>`). File paths
  are up to four parts of letters, digits, `.`, `_` and `-`. Pictures are PNG or JPEG.
- A layer needs a `source`; a `mask` must name a `<mask>` asset; a particle `source` is an image or
  frame sequence; an effect has a `type` or a `source` (a `kind="effect"` shader), not both.
- A value is either written out or a whole `$id`; `$id` must suit the attribute for every value the
  setting can take. `"$strength 14"` is refused: put the parts in separate slots.
- A motion's `property` must be one that moves (`attributes.md`, **Moves**), and one `<keys>` or
  `<drive>` at most sets each property. An effect's properties move as `<effect id>.<name>`, so
  give it an `id`.
- `when` takes one condition: a data name, `$id`, either with `=`/`!=` a word or value, or `!` in
  front. Settings' own `when` may only name other settings.
- Limits: 64 nodes, 4 levels of groups, 8 effects a node and 32 in all, 8 emitters and 20 000
  particles, 32 pictures, 1 video, 4 Lottie animations, 8 shaders, 32 settings, 512 MB of textures.

## Design

- **Work at the screen's shape.** The stage covers each screen and is cut at its sides on other
  shapes; keep faces and focal points away from the stage's edges. Corner marks belong on
  `space="screen"`.
- **Gentle motion reads as alive; big motion tires.** A lock screen is looked at for seconds, often
  in the dark: keep periods long (3 to 40 seconds), amplitudes small, and leave most of the picture
  still.
- **Characters:** `bend` with a mask over the hair or hem, `breathe` with a mask over the chest,
  blinking as a frame sequence with `gap`. Paint masks soft, over what moves and where it moves to.
  Split widely swinging parts into their own layers.
- **Depth:** give far layers a low `depth` and near ones a high one, and move the camera slowly.
  Overlays that cover the screen say `depth="0"`.
- **Give the user settings** for what they will want to change: weather on or off and how much,
  colours, how strongly things move. Group them, and hide dependent ones with `when`.
- **Keep it fast:** size pictures to the stage, keep textures under 300 MB, prefer a few effects on
  the parts that need them to effects over everything. `scene_check` reports the video memory and
  warns about oversized pictures.
- **Shaders:** a layer shader returns straight colour; an effect shader reads premultiplied
  `source` and returns premultiplied colour. Write `main(float2 uv : TEXCOORD0)` only.

## Checking and previewing

With a build of AnimeLogon (`cmake --preset x64`, then
`cmake --build --preset x64-release --target scene_check scene_preview scene`; the programs are in
`build\x64\bin\Release\`):

```powershell
scene_check my-scene                          # the reader's verdict, with the line of each error
scene_check --pack my-scene my-scene.alscene  # checked, then packed as a stored zip
scene_preview my-scene still.png --time 3     # the scene at 3 s, drawn by AnimeLogon's renderer
scene_preview my-scene frames --frames 8 --fps 4 --width 2560 --height 1080 petals=false
```

`scene_preview` imports the scene as AnimeLogon does, in the sandboxed renderer, and draws it at
full quality. `--day`, `--wake`, `--month`, `--battery`, `--progress` and `--state` set the inputs.
Look at the pictures before you deliver.
