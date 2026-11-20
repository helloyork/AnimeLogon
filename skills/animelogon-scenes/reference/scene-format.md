# The scene format

A scene is an animated wallpaper described by one XML file, `scene.xml` (format 2), and the
pictures, masks, video, Lottie animations and shaders it names. Every frame is worked out from
the moment and the inputs alone, so any moment can be drawn directly: the still picture, a
preview at a chosen time and the lock screen all draw the same thing. Nothing in a scene runs
code but its shaders, which are checked against a fixed contract and run in a sandbox; there are
no scripts, expressions, links or text.

Every attribute of the nodes, particles and built-in effects, the data names and the limits are in
[attributes.md](attributes.md), generated from AnimeLogon's reader.

## The package

A scene travels as an `.alscene`: a zip of stored (uncompressed) entries holding `scene.xml` at
its root and the files it names, by paths of up to four parts of letters, digits, `.`, `_` and `-`,
such as `art/sky.png`. `scene_check --pack <folder> <out.alscene>` writes one.

| File | Kinds |
|---|---|
| Pictures and masks | PNG or JPEG. Imported as compressed textures (BC7; BC4 for masks). |
| Video | MP4, MOV, MKV or WebM, transcoded as AnimeLogon's video wallpapers are. |
| Lottie | JSON, drawn with ThorVG. Expressions are not run. |
| Shaders | HLSL source, compiled when imported (see Shaders below). |

When it is imported, AnimeLogon draws the scene once in its sandboxed renderer at the moment `still`
for the still picture and thumbnail, and measures how fast this PC draws it.

## `<scene>`

```xml
<scene format="2" name="Snowfall" author="Your name" width="1920" height="1080" still="12">
  <settings>...</settings>
  <assets>...</assets>
  <camera>...</camera>
  ...nodes...
</scene>
```

| Attribute | Value |
|---|---|
| `format` | `2` |
| `name` | 1 to 64 characters. |
| `author` | Up to 64 characters. |
| `width`, `height` | The stage: the scene's own size in points, 16 to 16384. Default 1920 x 1080. |
| `still` | The moment, in seconds, the still picture and thumbnail show. Default 0. |

**The stage covers each screen**, scaled up or down until it fills it, so on a screen of another
shape its sides (or top and bottom) are cut. Keep what matters away from the stage's edges, or pin
it to the screen with `space="screen"`. With several screens, each draws the scene at its own
shape, at the same moment.

## Assets

```xml
<assets>
  <image id="sky" file="art/sky.png"/>
  <image id="pixel" file="art/pixel.png" exact="true"/>
  <mask id="hair" file="art/hair-mask.png"/>
  <frames id="blink"><frame file="art/eyes-0.png"/><frame file="art/eyes-1.png"/></frames>
  <video id="sea" file="sea.mp4"/>
  <lottie id="badge" file="badge.json"/>
  <shader id="aurora" kind="layer" file="aurora.hlsl"/>
  <shader id="ripple" kind="effect" file="ripple.hlsl"/>
</assets>
```

- `<image>`: a picture. `exact="true"` keeps it uncompressed (pixel art, fine gradients); it takes
  four times the memory.
- `<mask>`: a grey picture. White is where a mask lets something through, black where it does not;
  greys are in between. Paint soft edges.
- `<frames>`: 1 to 120 pictures played in order (blinking eyes, a flame). `exact` is taken here too.
- `<video>`: at most one in a scene.
- `<lottie>`: at most four.
- `<shader>`: `kind="layer"` draws a layer; `kind="effect"` is an effect on a node. At most eight.

Ids are letters, digits, `-` and `_`, starting with a letter, and unique across the scene.

## Nodes

Nodes are drawn in the order written, later ones on top.

| Element | Draws |
|---|---|
| `<layer source="...">` | A picture, frame sequence, video, Lottie animation or layer shader. |
| `<fill color="...">` | A colour, or a gradient with `colors`. |
| `<particles>` | A particle emitter (see Particles). |
| `<group>` | Its child nodes, moved, scaled, turned and faded together. Groups nest up to four deep. |

**Placing a node.** A node has a box. `x` and `y` place the box's `anchor` point: on the stage, or
within its group. The box's size is `width` and `height`; left out, a layer takes its source's size,
a fill or emitter the box it is in, and a group the extent of its children as written; with only one
of the two written, the other keeps that natural shape. `full="true"` makes the node cover the box
it is in (the stage, at the top level), as `fit` says. `rotation` and `scale` turn and grow the box
about its `pivot`.

**Groups** scale their children with their box: a group 980 points tall whose children reach 490
points draws them twice their size. A group with an `opacity` below 1, a `blend` other than
`normal` or a `mask` is drawn as one picture first and then composited, so its children do not
show through each other.

**Pinned to the screen.** `space="screen"` places a node against the screen instead of the stage,
so it is never cut: `anchor` picks the screen's corner or edge, and `x` and `y` are in percent of
the screen's height from there. Its size still follows the stage's scale.

**Masks.** `mask="id"` hides the node where the mask is black. The mask covers the node's box.

**Conditions.** `when` on a node or an effect shows it only while a condition holds: a data name
(`media.state=playing`, `clock.period!=night`, `battery.level`) or a setting (`$snow`,
`$style=night`, `!$stars`). One condition an element.

## The camera

```xml
<camera>
  <wander property="x" amplitude="24" period="40"/>
  <drive property="scale" by="wake" from="1" to="1.04"/>
</camera>
```

At most one `<camera>`. It draws nothing; its motions move `x`, `y`, `scale` and `rotation`, and
each top-level node follows by its `depth`: 0 stays put, as if far away; 1 moves fully. Different
depths give parallax. A fill or overlay that covers the stage moves too unless it says
`depth="0"`, and then an edge of the screen shows past it; `scene_check` warns about this.

## Motions

Motions sit inside a node (or the camera) and move one of its properties: an attribute whose
**Moves** column in [attributes.md](attributes.md) says yes, an effect's attribute as
`<effect id>.<attribute>` (`glow.strength`), a layer shader's parameter as `param.<slot>`, an
effect shader's as `<effect id>.<slot>`, or a Lottie layer's slot as `slot.<id>`.

| Element | Does |
|---|---|
| `<keys property duration repeat delay gap>` with `<key at value ease>` | Sets the value from key frames. |
| `<wave property amplitude period phase>` | Adds a sine. |
| `<wander property amplitude period>` | Adds a smooth random drift. |
| `<drive property by from to range>` | Sets the value from data or a setting. |

A property takes at most one `<keys>` or `<drive>`; any number of `<wave>` and `<wander>` add on
top. Values and amplitudes may be `$id`.

- **`<keys>`**: `duration` in seconds; `repeat` is `once` (holds the last key), `loop` (default) or
  `bounce` (forwards and back); `delay` waits before the first run. `gap="3-6"` rests at the first
  key for a different time from 3 to 6 seconds between runs, worked out from the moment, so it is
  the same every time that moment is drawn. Keys go in order of `at`, from 0 to `duration`. A key's
  `ease` shapes the way from it to the next key: `linear` (default), `smooth`, `in`, `out`, `step`
  (holds, then jumps), or `bezier` with `bezier="x1 y1 x2 y2"`.
- **`<wave>`**: `amplitude` either side, `period` in seconds, `phase` 0 to 1.
- **`<wander>`**: drifts up to about `amplitude` either side, changing direction every `period`
  seconds or so.
- **`<drive>`**: maps `by`, a number data name (`audio.bass`, `wake`, `clock.day`) or a number
  setting (`$amount`), onto `from`..`to`. `range="a b"` is the input range, by default the setting's
  `min` and `max`, 1 to 12 for `date.month`, and 0 to 1 otherwise. Sound comes in already smoothed.

## Effects

```xml
<layer source="hero">
  <effect type="bend" mask="hair" strength="0.5" period="3.2" root="top"/>
  <effect id="glow" type="glow" mask="lamp" radius="0.2" color="#FFC070"/>
  <drive property="glow.strength" by="audio.bass" from="0.6" to="1.4"/>
</layer>
```

Effects sit inside a node, up to eight, and run in the order written on the node's own picture.
Each can take a `mask` (only where it is white), a `when`, and an `id` so motions can move its
attributes. The built-in effects and their attributes are in [attributes.md](attributes.md).

How effects are drawn, which matters when you design with them:

- **Distances are fractions of the node's box height** (a bend's, of the box's length from its
  root), so an effect looks the same at any size.
- **The node is drawn into its own picture**, at the size it covers on the screen, with a margin for
  what its effects move or spread outward; then the effects run in the node's own frame (a turned
  node's bend still hangs from its own top) and the result is composited. A group with effects is
  drawn whole into that picture: children outside its box and margin are cut.
- **A `full` node repeats its edges** outward instead of reading transparency there, so blurring or
  waving a backdrop never shows what is below at the screen's edges.
- **Masks on moving effects** scale the movement, so edges blend instead of tearing. What moves
  outside the mask is cut off: paint the mask over what moves and over where it moves to.
- **Blur, glow and rays** work at a lower resolution at the user's lower qualities (half at high and
  automatic, a quarter at medium and low) and look about the same.

**Bringing a character to life** needs no skeleton: `bend` with a mask over the hair or a hem,
`breathe` with a mask over the chest, and a frame sequence with `gap` for blinking. Split large
swinging parts into their own layers.

### Effect shaders

```xml
<effect id="water" source="ripple" mask="pond">
  <param slot="0" value="$strength"/>
  <param slot="1" value="14 1.5"/>
</effect>
```

`source` names a `kind="effect"` shader. `<param slot="0".."15" value>` fills `param[slot]`, a
number, up to four numbers, or a colour; a value is either written out or a whole `$id`.

## Layer shaders and Lottie

A layer whose source is a `kind="layer"` shader draws its box with it; `<param slot value>` inside
the layer fills `param[slot]`. A Lottie layer is drawn at the size it covers; `<slot id value>`
inside it sets the animation's slots (colours, numbers) by their ids in the file.

## Shaders

Shaders are HLSL for pixel shader model 5.0, compiled in AnimeLogon's sandbox when imported. Write
only `main`; AnimeLogon puts these declarations before it:

```hlsl
cbuffer Scene : register(b0) {
    float time; float2 resolution; float day;
    float battery; float progress; float playing; float wake;
    float4 audio;
    float month; float3 reserved;
    float4 param[16];
};
Texture2D<float4> source : register(t0);
Texture2D<float4> mask : register(t1);
SamplerState linearClamp : register(s0);
```

```hlsl
float4 main(float2 uv : TEXCOORD0) : SV_Target { ... }
```

- `uv` runs from (0, 0) at the box's top left to (1, 1) at its bottom right.
- `time` is seconds since the scene started. `resolution` is the box's size in pixels. `day` is 0 to 1 through the local day,
  `battery` and `progress` are 0 to 1 or -1 when unknown, `playing` is 2 playing, 1 paused, 0 none,
  `wake` 0 to 1, `audio` is level, bass, mid and treble 0 to 1, `month` 1 to 12.
- **A layer shader** reads nothing but those numbers and returns straight (not premultiplied)
  colour; the layer's `opacity`, `blend` and `mask` apply as usual. `blend="add"` makes it light.
- **An effect shader** also reads `source`, the node's picture so far within its box, premultiplied
  by alpha, and `mask`, the effect's mask over the box (white everywhere when the effect has none),
  both at `uv`. It returns premultiplied colour for the box.
- Nothing else may be bound: other textures, buffers or samplers are refused, as is a shader that
  does not compile. `scene_check` compiles shaders against this contract.

## Particles

```xml
<particles id="petals" preset="petals" source="petal" rate="$amount" emit="top" wind="20" seed="7" depth="0.8"/>
```

An emitter gives off particles from its box: just outside an edge (`emit="top"` and so on), at its
centre, along its middle across, or anywhere in it. Left without a size, the box is the one it is
in: the whole stage at the top level. Each particle's place is worked out from the moment, its
number and the seed, so particles are the same every time a moment is drawn, and some are already in
flight at the scene's first moment.

At most `max` particles are alive at once (`rate` x `lifetime` when that is fewer); raise `max` for
dense rain. `preset` fills in what the element leaves out: `rain`, `snow`, `petals`, `fireflies`,
`stars`, `dust` (values in [attributes.md](attributes.md)). Each particle is blended on its own by
the emitter's `blend`, so `add` brightens where they overlap, and an emitter can carry effects (a
`glow` on fireflies). The user's particle density setting thins every emitter.

## Data

What a `when` and a `<drive>` can read: the time and clock, the date, the battery and power, what
is playing and how far through it, the sound playing on the PC, and how far the lock screen has
faded after the user woke it. The names are in [attributes.md](attributes.md). A scene shows no
text; put text over it with a component.

## Settings

```xml
<settings>
  <group label="Weather">
    <toggle id="petals" label="Falling petals" default="on"/>
    <number id="amount" label="Petals a second" min="2" max="40" step="1" default="12" when="$petals"/>
  </group>
  <color id="lantern" label="Lantern colour" default="#FFB86B"/>
  <choice id="style" label="Time of day" default="day">
    <option value="day" label="Day"/>
    <option value="night" label="Night"/>
  </choice>
</settings>
```

Settings work as in components: `toggle`, `number`, `choice`, `color` and `colors`, each with an
`id`, a `label`, a `default` and an optional `detail`, and any attribute of the scene may be `$id`.
A toggle's value is `true` or `false` unless it gives `on` and `off`. On top of that:

- `<group label>` shows the settings inside it together in the AnimeLogon app. Groups do not nest.
- `when="$id"`, `when="$id=value"` or `when="!$id"` on a setting shows it only while another
  setting says so. It changes only what the app shows, never the scene.
- A choice's option values can be asset ids, so `source="$outfit"` changes a layer's picture.
- `<image id label default>` names an image asset the user can replace with one of the picture
  wallpapers in their library. A node whose `source` is `$id` keeps the box the default picture
  gives it and shows the chosen picture there, its middle cut to cover the box. An exported theme
  carries the default, not the user's picture.

A theme keeps the user's values; the same scene can appear in several themes with different values.

## Making scenes fast

Scenes often show on laptops' integrated graphics, at the lock screen, on battery.

- **Pictures** take video memory once compressed: about a byte a pixel (half for a mask, four for
  `exact`), a third more for their smaller copies. Keep the total at 300 MB or less; over 512 MB is
  refused. A picture far larger than it is drawn costs memory for nothing: size it to the stage.
- **What does not move costs little.** A frame where nothing changes is not drawn again, and a node
  whose effects and properties do not change keeps its result. Motions, moving effects, particles,
  video and Lottie make frames.
- **Effects** each cost about one more pass over the node's area; `rays` costs the most. Large
  particles that overlap a lot cost more than many small ones.
- The user chooses the quality, frame rates for mains, battery and energy saver, a video memory
  limit and particle density. At the automatic quality, a PC that falls behind lowers the frame
  rate, then the resolution, and finally shows the still picture.

## Checking and previewing

`scene_check <folder|scene.xml|.alscene>` reads the scene as the importer does, naming the line of
each problem, compiles its shaders, and prints what it holds and the video memory its pictures take,
with warnings for what makes a scene slow or blurry.

`scene_preview <folder|.alscene> <out.png> [--time seconds] [--width N --height N]
[--frames N --fps N] [--day 0-1] [--wake 0-1] [--month 1-12] [id=value ...]` imports the scene
and draws it with AnimeLogon's own sandboxed renderer: one picture, or a folder of frames.
