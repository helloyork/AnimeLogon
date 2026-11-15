---
name: animelogon-components
description: Write, fix and package AnimeLogon lock-screen components, the XML files (root element <component>) that place clocks, dates, battery, now playing, countdowns and audio spectra over a theme's wallpaper, and the theme packages (.altheme) that carry them. Use when asked to create or change an AnimeLogon component or widget, turn a design or screenshot into one, find out why AnimeLogon refuses a component file, or build an .altheme to share.
license: MIT. See LICENSE in this folder.
---

# AnimeLogon components

A component is a small, static XML file. AnimeLogon reads it with a strict reader and draws it on
the lock screen. It declares panels of text, progress bars and audio spectra, the data they show
(`{time}`, `{battery.percent}`, `{media.title}`...) and settings the user can change in the
AnimeLogon app. Nothing in it computes: there are no scripts, expressions, images or links.

Read these as you need them:

- [reference/component-format.md](reference/component-format.md): the file, its structure,
  settings and `$` bindings, data and conditions, layout, element keys. Read it before writing a
  component from scratch.
- [reference/attributes.md](reference/attributes.md): every attribute of every element with its
  values, default and meaning, every data name, and the limits. Generated from AnimeLogon's reader.
- [reference/theme-package.md](reference/theme-package.md): `theme.xml` and `.altheme` packages.
- [examples/](examples/): complete components that AnimeLogon accepts.

## Workflow

1. **Pin down the brief.** What it shows, where on the display, how large, and what the user should
   be able to change (position, size, font, colour, parts on or off). Ask only what you cannot
   decide from the request; a clock in the top-left corner needs no questions.
2. **Start from something close.** Copy the nearest example below, or a built-in component
   (`component_check --builtin battery`), rather than starting from nothing.
3. **Write the XML** by the rules below. Check each attribute's values in `attributes.md`.
4. **Check it** (next section) and fix every error. Errors name the line.
5. **Look at it**, when you can build AnimeLogon: draw it over a picture with `skin_preview`.
6. **Deliver it.** Give the user the `.xml` and say how to add it (end of this file), or pack it
   into an `.altheme` with a theme (`reference/theme-package.md`).

| Example | Shows |
|---|---|
| `examples/minimal.xml` | The smallest useful component: the time. |
| `examples/corner-clock.xml` | Clock with AM/PM, weekday and date; position, size, font, colour and backdrop settings. |
| `examples/now-playing.xml` | Title, artist and a progress line in a plate; hidden when nothing plays. |
| `examples/battery.xml` | Battery icon, charge and time left; nothing on a PC without a battery. |
| `examples/countdown.xml` | Days to a date the user picks, "Today" on the day, hidden after it. |
| `examples/spectrum.xml` | Audio spectrum with a colour gradient and style choices. |
| `examples/two-panels.xml` | One component with two panels in different corners. |
| `examples/theme/` | A theme package folder: `theme.xml` placing built-in and own components. |

## Skeleton

```xml
<?xml version="1.0" encoding="utf-8"?>
<component format="1" name="My clock" author="Your name">
  <settings>
    <choice id="position" label="Position" default="top-left">
      <option value="top-left" label="Top left"/>
      <option value="bottom-right" label="Bottom right"/>
    </choice>
    <number id="size" label="Size" default="8" min="3" max="20" step="0.5"/>
    <color id="color" label="Color" default="#FFFFFF"/>
  </settings>
  <panel anchor="$position" size="$size" margin="6">
    <line>
      <text value="{time}" weight="semibold" color="$color"/>
    </line>
    <line gap="0.15">
      <text value="{date}" size="0.25" color="$color"/>
    </line>
    <shadow blur="0.08" opacity="0.35" y="0.02"/>
  </panel>
</component>
```

## Rules the reader enforces

Most refusals come from these. The full rules are in `component-format.md`.

- Root `<component format="1" name="...">`; `name` is required, `author` is optional.
- Only elements and attributes. No text between tags: words go in `value` attributes. Comments
  are fine. No DOCTYPE, CDATA or namespaces (`xmlns`, `xsi:` are refused).
- Unknown elements and unknown attributes are errors, not ignored. Attribute names are exact and
  lowercase: `offset-x`, `track-opacity`, `idle-opacity`, `enter-delay`.
- `<settings>` comes once, before the first `<panel>`. Each setting needs `id`, `label` and a valid
  `default`. Ids are lowercase letters, digits and `-`; `ref` and `visible` are reserved.
- Order: `<component>` holds `<settings>` and 1–8 `<panel>`; a panel holds 1–8 `<line>`, up to 4
  `<shadow>` and one `<backdrop>`; a line holds 1–8 `<text>`, `<bar>` or `<spectrum>`, which hold
  nothing (write them as `<text .../>`).
- `<text>` and `<bar>` need `value`. A bar's value is exactly `{battery.level}` or
  `{media.progress}`. A text's braces must name text data: `{media.progress}` in a text is refused.
- A literal `{` or `}` cannot appear in a text. `&`, `<` and `"` are written `&amp;` `&lt;` `&quot;`.
- `$id` binds an attribute to a setting, and every value the setting can take must suit the
  attribute: a `<number>` setting's `min`–`max` must lie within the attribute's range; each option
  of a `<choice>`, and a `<toggle>`'s `on` and `off`, must be valid values for it. `label` cannot
  be bound. A toggle's `default` is `on` or `off`, not one of its values.
- `when` takes one condition: `name`, `!name`, `name=word` or `name!=word`, where `=` and `!=` only
  work on state data (`media.state`, `battery.state`, `power.source`, `until.state`). No `and`/`or`:
  nest a `when` on the panel, the line and the text instead.
- Numbers are plain decimals within the attribute's range (`0.35`, not `35%` or `.35em`).
  Colours are `#RRGGBB` or `#RRGGBBAA`.

## Design

- **Sizes are in em.** A panel's `size` is its em in percent of the display's height (a large clock
  is 8–14, a corner widget 2–4). Every other length is in em, so the component scales with
  the display and with the user's size setting.
- **Give the user settings** for what they will want to change: position (`anchor`), size, font and
  colour at least; toggles for optional parts (`show="$icon"`), a toggle for a backdrop
  (`opacity="$plate"` with `on="0.4" off="0"`). Labels and details appear as written in the
  AnimeLogon app, in every language: write them in your audience's language.
- **Stay readable on any wallpaper:** a soft `<shadow>`, and a `<backdrop>` the user can turn on.
- **Hide what has no data.** A text whose data is empty draws nothing, but its neighbours still
  show: use `when` so a panel or line disappears as a whole (`when="media.state"`,
  `when="power.source"`).
- **Fonts** must be installed for all users to appear on the lock screen. Icons come from
  `Segoe Fluent Icons` (Windows 10 falls back to Segoe MDL2 Assets): `{battery.glyph}`,
  `{media.glyph}`, or a code point such as `&#xE95F;`.
- **When revising a component people already use, keep existing elements where they are.** The
  user's adjustments and a theme's `<set>` values are stored by position (`p1.l2.t1` is the first
  text of the second line of the first panel). Add new texts at the end of a line and new lines
  after the existing ones, or adjustments land on other elements.
- **Write `hang="true"`** on a text such as AM/PM that should sit beside the time without moving it
  when lines are aligned; the AnimeLogon app offers the choice only on texts that write `hang`.

## Checking and previewing

With a build of AnimeLogon (`cmake --preset x64`, then
`cmake --build --preset x64-release --target component_check skin_preview`; the programs are in
`build\x64\bin\Release\`):

```powershell
component_check my-clock.xml                 # the reader's verdict, with the line of each error
component_check theme-folder                 # a package folder: theme.xml, components\*.xml
component_check --pack theme-folder out.altheme
component_check --builtin media              # a built-in component's XML, to start from
skin_preview wallpaper.jpg out.png --skin my-clock.xml --data sample size=12
```

`skin_preview` draws the component over any picture. `--data sample` fills battery and media with
sample values (`paused`, `charging`, `mains`, `idle` show other states); `--time 21:41`,
`--date 2027-01-01`, `--hours 12` and `--locale ja-JP` set the clock; `setting=value` and
`p1.l1.t1.weight=300` set settings and adjustments. Look at the picture before you deliver.

Without a build, AnimeLogon itself checks the file when it is added (below) and shows the reader's
message, with its line, when it refuses it. To try components without touching the user's themes,
open a sandbox: `"C:\Program Files\AnimeLogon\animelogon.exe" --sandbox D:\component-test`. That
window works on separate data in the folder, adds components without administrator approval, and
shows them in its previews; the real lock screen does not use it.

## Adding it to AnimeLogon

In the AnimeLogon app, open a theme with **Customize**, select **Add component** under
**Components** and choose **A component .xml file**. Windows asks for administrator approval once:
components are kept where only administrators can write, because the lock screen shows them before
anyone signs in. A theme holds up to 8 components. To share a theme with its components, export it,
or build an `.altheme` as `reference/theme-package.md` describes.
