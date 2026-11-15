# Themes and theme packages

A theme is one wallpaper and up to 8 components placed over it. A theme package (`.altheme`)
carries a theme with its wallpaper and component files, for sharing. AnimeLogon exports and
imports them; you can also build one by hand.

## theme.xml (format 1)

```xml
<?xml version="1.0" encoding="utf-8"?>
<theme format="1" name="Evening" author="Your name">
  <wallpaper ref="wallpaper.mp4" fit="fill" brightness="0.8" blur="0"/>
  <wake style="slide" duration="0.6"/>
  <component id="clock" ref="clock">
    <set key="position" value="bottom-left"/>
    <set key="p1.l1.t1.weight" value="600"/>
  </component>
  <component id="day" ref="components/weekday.xml"/>
  <component id="music" ref="media" visible="false"/>
</theme>
```

- `<theme format name author?>`: `format` is `1`; `name` 1–40 characters; `author` up to 40. At most
  64 KB. Unknown elements and attributes are refused.
- `<wallpaper ref fit brightness? blur?>`, exactly once.
  - `ref`: `wallpaper.mp4`, `wallpaper.png`, `wallpaper.jpg` or `wallpaper.webp` in the package;
    `default` for AnimeLogon's built-in wallpaper; `none` for black.
  - `fit`: `fill` (cover the display, cropping), `fit` (whole wallpaper, with bars) or `stretch`.
  - `brightness`: 0.1 to 1, default 1. `blur`: 0 to 5 percent of the display's height, default 0.
- `<wake style? duration?>`, at most once: how the lock screen gives way to the sign-in screen.
  `style` is `fade`, `zoom`, `slide` or `cut`; `duration` 0.1 to 2 seconds. Default: a fade over
  0.5 seconds.
- `<component id ref visible?>`, up to 8.
  - `id` names this placement: a lowercase letter, then up to 15 lowercase letters, digits or `-`.
    It is unique in the theme and is not `wallpaper` or `fit`. One component may be placed twice
    under different ids.
  - `ref`: a built-in component (`clock`, `battery`, `media`, `countdown`, `spectrum`) or a file in
    the package, `components/<name>.xml`, where `<name>` is 1–32 characters of lowercase letters,
    digits, `_` and `-`, starting with a letter or digit.
  - `visible`: `true` (default) or `false`.
- `<set key value>`, up to 128 per component: a setting id of the component with one of its values,
  or an adjustment such as `p1.l1.t1.weight` with a value of that attribute
  ([component-format.md](component-format.md#element-keys-and-adjustments)). Values the component
  does not take are ignored when the theme is shown.

## The .altheme file

An `.altheme` is a zip file whose entries are all stored, without compression. It holds exactly
these entries, with these lowercase names, and nothing else:

| Entry | |
|---|---|
| `theme.xml` | required, at most 64 KB |
| `wallpaper.mp4`, `.png`, `.jpg` or `.webp` | the one `theme.xml` names; none when it names `default` or `none` |
| `components/<name>.xml` | each component file `theme.xml` names, and no others; up to 8, each at most 64 KB |
| `preview.png` | optional, at most 16 MB |

AnimeLogon refuses a package with compressed entries, folders as entries, other files, encryption,
or zip64. The whole file is at most 1 GB; a picture at most 256 MB.

## Building one

Lay the package out in a folder:

```text
evening\
  theme.xml
  wallpaper.mp4            optional
  components\
    weekday.xml
```

Then pack it, by one of:

- `component_check --pack evening Evening.altheme`, from a build of AnimeLogon. It checks the
  folder first and writes a package AnimeLogon accepts.
- In Command Prompt, from inside the folder, name every file (not the `components` folder itself,
  which would add a folder entry):

  ```bat
  tar -c --format zip --options zip:compression=store -f ..\Evening.altheme theme.xml components\weekday.xml
  ```

- In 7-Zip: archive format zip, compression level **Store**, then rename the file to `.altheme`.

`component_check Evening.altheme` checks a finished package as AnimeLogon's importer does, except
that it does not decode the wallpaper; it also reports `<set>` values the components do not take.

## Importing and exporting

- Select **Import…** in the AnimeLogon app and choose **Theme package**. Windows asks for
  administrator approval once. The theme and its components get new ids; component files are
  stored as AnimeLogon read them, without comments.
- To share a theme as it is now, open its menu in the theme library and select **Export theme
  package…**. The
  user's changes are written into `theme.xml` as `<set>` values, and the wallpaper and the
  components it uses go with it.
