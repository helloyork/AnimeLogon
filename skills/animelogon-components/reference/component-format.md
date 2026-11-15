# Component format 1

A component is what a theme places over its wallpaper: one XML file whose root element is
`<component>`. This page describes format 1, which AnimeLogon 1.0 reads. Every attribute, its
values and its default are listed in [attributes.md](attributes.md).

## The file

- UTF-8, with or without a byte order mark, at most 64 KB.
- A small part of XML: elements and attributes, comments, and an optional XML declaration.
  DOCTYPE, CDATA, processing instructions, namespaces and text between elements are refused.
- Element and attribute names are case-sensitive. An element or attribute AnimeLogon does not know
  is an error, not ignored.
- In attribute values, the entities `&amp;` `&lt;` `&gt;` `&quot;` `&apos;` and character
  references such as `&#xE95F;` are read; a tab or line break counts as a space.
- AnimeLogon stores the component as it read it: comments, the declaration and the layout of the
  file are not kept.
- When a file is refused, the message names the line and what is wrong, for example
  `line 12: unknown attribute colour on <text>`.

## Structure

```text
<component format="1" name author?>
  <settings>                      optional, once, before the first panel
    <choice> <toggle> <number> <color> <font> <date> <colors>      up to 32
  </settings>
  <panel>                         1 to 8
    <line>                        1 to 8
      <text/> <bar/> <spectrum/>  1 to 8, drawn left to right in this order
    </line>
    <shadow/>                     up to 4
    <backdrop/>                   up to 1
  </panel>
</component>
```

- `format` must be `1`. `name` (1–40 characters) is required; `author` (up to 40) is optional. Both
  appear only in the AnimeLogon app.
- `<text>`, `<bar>`, `<spectrum>`, `<shadow>` and `<backdrop>` hold no elements.
- `<shadow>` and `<backdrop>` may stand anywhere among a panel's lines.

## Settings

Settings are what the user can change in the AnimeLogon app. The window shows them in the order
written. The values the user chooses are kept in AnimeLogon's settings, never in the file.

| Element | Value | Attributes besides `id`, `label`, `detail`, `default` |
|---|---|---|
| `<choice>` | one option's `value` | none; holds 1–32 `<option value label/>` |
| `<toggle>` | `on` or `off` | `on`, `off`: the values the bound attributes get |
| `<number>` | a number from `min` to `max` | `min`, `max` (`min` < `max`), `step` (positive, default 1) |
| `<color>` | `#RRGGBB` or `#RRGGBBAA` | none |
| `<font>` | a font family, or empty for the default | none |
| `<date>` | `YYYY-MM-DD` | none |
| `<colors>` | colour stops | none |

- `id`: 1–32 characters of `a`–`z`, `0`–`9` and `-`, unique. `ref` and `visible` are reserved.
- `label`: 1–40 characters, shown as the setting's name. `detail`: up to 80 characters, shown
  under it. Both are optional to translate: they appear as written in every language. (A label
  identical to one of AnimeLogon's own Chinese labels, such as `位置`, is shown in its translation.)
- `default` is required and must be a value of the setting.

### Binding attributes to settings

Any attribute except `label` may be written `$id`: it takes the value of setting `id`. Every value
the setting can take must suit the attribute, or the file is refused:

- a `<choice>`: each option's `value` must be a valid value of the attribute;
- a `<toggle>`: its `on` and `off` must both be valid values of the attribute;
- a `<number>`: only for number attributes, and `min`–`max` must lie within the attribute's range
  (for `bands`, whole numbers and a whole `step`);
- `<color>`, `<font>`, `<date>`, `<colors>`: only for attributes of that kind (a `<date>` for a
  panel's `until`).

```xml
<toggle id="seconds" label="Show date" default="on" on="true" off="false"/>
<line show="$seconds">...</line>

<toggle id="plate" label="Background plate" default="off" on="0.4" off="0"/>
<backdrop shape="plate" opacity="$plate"/>
```

## Text and data

A `<text>`'s `value` is words with data names in braces: `{time}`, `{battery.percent}%`,
`{until.prefix} {until.days} {until.unit}`. At most 256 characters.

- Every name in braces must be a text name in the data table of [attributes.md](attributes.md).
  A `}` without its `{` is refused, so a text cannot show a brace.
- Data with no value is empty: `{media.title}` while nothing plays. A text that comes out empty
  draws nothing.
- A `<bar>`'s `value` is one fraction: `{battery.level}` or `{media.progress}`. A bar with no value
  is not drawn.
- A `<spectrum>` shows the sound the output device plays; it has no `value`.
- The words of data such as `{power.status}` and `{until.unit}`, and the clock's own words, follow
  the clock settings' language: Chinese, Japanese or English.

The clock's behaviour belongs to AnimeLogon's Settings, not to a component: 12 or 24 hours, the
AM/PM marker, the date style and the language apply to every component. With the date style set
to none, all date data is empty.

Where the data comes from:

- **Clock**: the PC's time and regional format.
- **Battery** (`battery.*`, `power.*`): read by the lock screen itself.
- **Media** (`media.*`): what plays in the signed-in user's session, on that user's lock screen.
  On the sign-in screen at startup, before anyone signs in, nothing plays.
- **Countdown** (`until.*`): days from today to the panel's `until` date.

## Conditions

`when` on a panel, line, text, bar or spectrum draws it only while its condition holds.

| Condition | Holds when |
|---|---|
| `name` | the data has a value: a text is not empty, a fraction is known, a state is not `none` |
| `!name` | the data has no value |
| `name=word` | a state is `word` |
| `name!=word` | a state is not `word` |

- One condition per element: no `and`, `or` or brackets. To require several, put conditions on the
  panel, the line and the item: each must hold.
- `=` and `!=` work only on state data, with one of its words (see the data table).
- A line or item that is not drawn takes no room. A panel that has nothing to draw draws nothing,
  its backdrop included.

```xml
<panel when="media.state">                          <!-- only while something plays or is paused -->
<text value="{battery.remaining.long}" when="battery.state=discharging"/>
<text value="{until.today}" when="until.state=today"/>
```

## Colour stops

A spectrum's `colors`, and a `<colors>` setting, are one to eight colours at places along a
gradient: `#RRGGBB` or `#RRGGBBAA`, a space, and a place from `0%` to `100%`, separated by commas,
with the places in order.

```xml
<spectrum colors="#8FD3FF 0%, #C6B9FF 50%, #F3A6FF 100%"/>
<spectrum colors="#FFFFFF 0%"/>                    <!-- one colour: a plain colour -->
```

## Layout

- **em.** A panel's `size` is its em, in percent of the display's height. Sizes, gaps, shadows,
  bars and spectra inside the panel are in em. A text's `size` is relative: `0.25` is a quarter of
  the panel's em.
- **Placing a panel.** `anchor` names one of nine places. `margin` keeps the panel that far from the
  display edges the anchor is next to, in percent of the display's shorter side. `offset-x` and
  `offset-y` then move it, in percent of the display's width and height. The user can drag a panel
  in the AnimeLogon app; AnimeLogon keeps at least a quarter of it on the display.
- **Narrow displays.** A panel whose text is wider than the space between its margins is drawn
  smaller to fit. A line with a spectrum may span the display's whole width instead: a spectrum
  set wider than that is shortened to the display's width, keeping its height, and lies flush with
  both edges. Its line's other items keep their size.
- **Lines** stack downwards. A line's `gap` is the distance from the line above's baseline to the
  top of this line's ink. `align` lines them up on the left, centre or right; `auto` follows the
  anchor's column.
- **Items** in a line follow each other from left to right. `space` is the room before an item; the
  first item drawn on a line gets none, so hiding the first item does not indent the line.
- **`hang="true"`** keeps a text (an AM/PM marker) beside the others but out of the alignment, so
  the other lines align with the time alone.
- **Order of drawing.** Shadows are drawn under the ink, the backdrop under both. A glow backdrop
  fades out around the ink; a plate is a rounded rectangle around it.
- **Entrance.** When the lock screen appears, each panel comes in by its `enter`, `enter-delay` and
  `enter-duration`.

## Element keys and adjustments

Every element has a key by its position, counted from 1 in document order among its siblings of
the same kind:

| Key | Element |
|---|---|
| `p1` | first `<panel>` |
| `p1.l2` | second `<line>` of the first panel |
| `p1.l2.t1` | first `<text>` of that line |
| `p1.l2.r1` | first `<bar>` of that line |
| `p1.l2.a1` | first `<spectrum>` of that line |
| `p1.s1` | first `<shadow>` of the first panel |
| `p1.b` | the first panel's `<backdrop>` |

An **adjustment** is a key and an attribute, such as `p1.l1.t1.weight`. The AnimeLogon app lets
the user adjust each element's adjustable attributes, and a theme can set them with `<set>`
([theme-package.md](theme-package.md)). Which attributes are adjustable is in
[attributes.md](attributes.md).

An attribute's value is the first of these that applies:

1. an adjustment of it, from the user or from the theme (the user's wins);
2. the value written in the file, or, for `$id`, the setting's value: the user's choice, else the
   theme's `<set>`, else the setting's `default`;
3. the attribute's default.

Adjustments and settings are stored by key and id. When you revise a component that is in use,
keep its settings' ids and its elements' positions: add texts at the end of a line and lines or
panels after the existing ones. Otherwise saved values apply to different elements, or are dropped.

## What a component cannot do

- Compute anything: no arithmetic, expressions, loops, functions or scripts.
- Show pictures, open links or fetch anything. Icons are characters of an icon font.
- Use font files: only fonts installed on the PC for all users.
- Take free text from the user. Settings hold colours, numbers, fonts, dates and fixed choices, so
  every word on the lock screen is written in the component or comes from the data.

## Where components are kept

AnimeLogon keeps components in `%ProgramData%\AnimeLogon\components\<id>\component.xml`, where only
administrators can write, because the lock screen shows them before anyone signs in. The built-in
components are `clock`, `battery`, `media`, `countdown` and `spectrum`. A component is added from a
theme's **Components** list (**Add component > A component .xml file**), which asks for
administrator approval, or arrives with a theme package.

## Compatibility

`format="1"` covers this page. Later versions of AnimeLogon may add elements and attributes to
format 1. Because unknown elements and attributes are refused, a component that uses them is
refused by earlier versions. A component of a later format, such as `format="2"`, is refused with
a message that it needs a newer AnimeLogon.
