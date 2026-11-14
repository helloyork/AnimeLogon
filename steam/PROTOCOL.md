# animelogon-steam protocol

`animelogon-steam.exe` is the settings app's connection to Steam, for the Steam Workshop. It is the
only AnimeLogon program that loads Steam's library, `steam_api64.dll`, and it loads it only from its
own folder. The settings app starts it when it needs the Workshop and talks to it over standard
input and output. Nothing on the logon screen depends on it.

It is built for x64 only (Valve ships no ARM64 library for Windows); on ARM64 it runs under
emulation. Steam's library is not part of this repository: the helper declares the parts of the API
it uses itself, in `src/steam_flat.h`.

## Starting it

```
animelogon-steam.exe [--appid <n>]
```

- `--appid` is the Steam app id. Without it the helper uses `kAppId` in `src/appid.h`, which is 0
  until Steam gives AnimeLogon an id; with 0, every request answers `no_appid`.
- It sets the `SteamAppId` environment variable to the app id, loads `steam_api64.dll` from its own
  folder, and connects to Steam at once. What went wrong, if anything, is what `status` reports.
- It is a console program without a window; start it with `CREATE_NO_WINDOW` and pipes for standard
  input and output. Standard error is used only for a wrong command line (exit code 2).
- It exits (code 0) when it is sent `quit` or its standard input closes, after disconnecting from
  Steam. Requests still waiting on Steam at that moment are not answered.

## Lines

- UTF-8, one JSON object per line, each ended by `\n` (a `\r` before it is ignored). A line may be up
  to 1 MiB.
- **Request:** `{"id": <integer>, "op": "<operation>", ...parameters}`. The `id` is the client's
  own, any whole number up to 2^53; it comes back on the answer.
- **Answer:** exactly one per request, `{"id": <n>, "ok": true, ...}` or
  `{"id": <n>, "ok": false, "error": "<code>", "message": "<English text>"}`. Steam answers in its
  own time, so answers can come in another order than the requests: match them by `id`.
  A line that is not a request is answered with `"id": null`.
- **Event:** `{"event": "<name>", ...}`, sent unasked, at any time between answers.
- **64-bit ids are strings of decimal digits**: Workshop item ids, Steam ids of authors. JavaScript's
  numbers cannot hold them. Every other number is a JSON number (sizes stay below 2^53).
- A request names an item with **`itemId`**; `id` is always the request's own. In answers and events,
  `id` inside an item, and the `id` of an event, are item ids.
- Parameters the helper does not know are ignored. Times are seconds since 1970 (UTC).

## Errors

| `error` | Meaning |
|---|---|
| `bad_request` | Not a JSON object, no whole-number `id`, or a parameter missing or wrong (the message says which). |
| `unknown_op` | No such operation. |
| `missing_dll` | `steam_api64.dll` is not beside the helper: not the Steam edition of AnimeLogon. |
| `no_appid` | The app id is 0. |
| `not_running` | Steam is not running, or was closed. The next request after 3 seconds tries again. |
| `init_failed` | Steam's library is unusable (an entry point missing, a version Steam refuses...) or Steam refused the connection. |
| `not_owned` | The Steam account signed in does not own AnimeLogon. |
| `steam_error` | Steam answered with a failure: `result` is Steam's `EResult` number, `message` its name (`"Fail"`, `"FileNotFound"`, `"LimitExceeded"`...). |
| `call_failed` | Steam could not complete or would not start the request (no connection, a refused field...). |
| `timeout` | Steam did not answer within 60 seconds (publishing has no time limit). |
| `not_found` | `details`: no such item, or it is hidden from this account. |
| `unsupported` | This `steam_api64.dll` is too old for the request (paging by cursor, content descriptors). |

Every operation but `status` and `quit` checks its parameters, then fails with `missing_dll`,
`no_appid`, `not_running`, `init_failed` or `not_owned` when that is the state, before asking Steam.

## Names

- **Content descriptors** (Steam's `EUGCContentDescriptorID`, what an author says an item contains):
  `nuditySexual` (1, some nudity or sexual content), `violenceGore` (2, frequent violence or gore),
  `adultOnlySexual` (3), `gratuitousSexual` (4), `generalMature` (5, general mature content). An
  unknown value is sent as its number in a string, e.g. `"7"`.
- **Visibility:** `public`, `friends`, `private`, `unlisted`.
- **Preview types:** `image`, `youtube` (the "url" is a YouTube video id), `sketchfab`,
  `environmentMapCross`, `environmentMapLatLong`, `clip`.
- **Upload stages:** `idle`, `preparingConfig`, `preparingContent`, `uploadingContent`,
  `uploadingPreview`, `committing`.

## Tags

Every tag a Workshop item of AnimeLogon can have is listed in [`tags.json`](tags.json), in English,
in four groups. That one list is what the settings app filters by, what publishing sets, and what
the Workshop's tags are set up from in Steamworks; adding a tag means adding it there, in Steamworks,
and to `res/locales` (`steam.tag.<the tag in lowercase, spaces as underscores>`, which is how the
settings app shows it: `Custom components` is `steam.tag.custom_components`).

| Group | Tags | Which |
|---|---|---|
| `type` | `Video`, `Picture` | The wallpaper: exactly one. |
| `resolution` | `1080p`, `1440p`, `4K`, `Ultrawide` | By the wallpaper's height: `1080p` from 1080 pixels, `1440p` from 1440, `4K` from 2160; none below 1080. `Ultrawide` as well when it is at least 2.2 times as wide as it is high. |
| `content` | `Sound`, `Components`, `Custom components` | `Sound`: the wallpaper has sound. `Components`: the theme shows components. `Custom components`: the `.altheme` carries components of its own (`components/<name>.xml` entries), not only the built-in ones. Publishing sets these from the theme and the package; the author does not choose them. |
| `rating` | `Everyone`, `Questionable`, `Mature` | The age rating: exactly one, chosen by the author when publishing. |

Tags are matched as they are written, so items are published with these spellings only.

**An item's age rating** is its strictest rating tag. An item without one counts as `Mature` (anyone
who owns the app can upload an item without the settings app), and so does an item with a content
descriptor, whatever its tags say. `query` and `details` give it as `rating`; `query` shows only
the ratings asked for.

## Item

What `query` and `details` give for an item:

```json
{"id": "3100000001", "title": "夜樱 Night Sakura", "description": "...", "owner": "76561198000000101",
 "preview": "https://...", "tags": ["Video", "4K", "Sound", "Everyone"], "descriptors": [], "rating": "Everyone",
 "votesUp": 1840, "votesDown": 32, "score": 0.97, "subscriptions": 25300, "favorites": 4100,
 "fileSize": 182000000, "timeCreated": 1760000000, "timeUpdated": 1762000000,
 "subscribed": true, "installed": true}
```

- `description` is as the author wrote it, with Steam's BBCode markup (`[b]...[/b]`, `[url=...]`).
  In `query` it is Steam's short description, the first 255 bytes (the helper drops a character cut
  in half); `details` gives the whole of it.
- `preview` is the URL of the main preview picture on Steam's servers; empty when there is none.
- `rating` is the item's age rating, as "Tags" decides it: `Everyone`, `Questionable` or `Mature`.
- `score` is from 0 to 1. `subscriptions` is the current subscribers, `favorites` the favourites.
- `fileSize` is the size of the item's files, in bytes.
- `subscribed` and `installed` are this computer's state of the item.
- `details` adds `"visibility"` and `"previews": [{"url", "type", "name"}]`, the additional previews.

## Operations

### status

```
→ {"id":1,"op":"status"}
← {"id":1,"ok":true,"protocol":2,"steam":"ok","appid":480,"loggedOn":true,"ownsApp":true}
```

- `steam`: `ok`, `not_running`, `missing_dll`, `no_appid` or `init_failed`; with `message` (English)
  when not `ok`.
- `protocol` is this document's version, raised when a request or answer changes incompatibly.
- When Steam was not running, `status` tries to connect again (at most every 3 seconds), so polling
  it notices Steam starting.

### query

Browses the Workshop: every item of the app that can be subscribed to.

```
→ {"id":2,"op":"query","sort":"trend","trendDays":7,"text":"sakura","requiredTags":["Video"],
   "excludedTags":["Sound"],"matchAnyTag":false,"ratings":["Everyone"],"page":1,"perPage":30}
← {"id":2,"ok":true,"total":128,"next":2,"items":[...],"hidden":1}
```

| Parameter | Default | |
|---|---|---|
| `sort` | `trend` | `trend` (most popular over `trendDays`), `recent` (newest first), `subscribed` (most subscribers ever), `rating` (top rated). |
| `trendDays` | 7 | 1 to 365, for `trend`. |
| `text` | none | Matched against titles, descriptions and tags. |
| `requiredTags`, `excludedTags` | none | Arrays of tag names. |
| `matchAnyTag` | false | An item needs any of `requiredTags` rather than all. |
| `ratings` | `["Everyone"]` | The age ratings shown, one or more of `Everyone`, `Questionable`, `Mature`. Steam leaves out the items tagged with the others; the helper leaves out each item whose rating (see "Tags") is not among them. An item with several rating tags shows only when all of them are asked for. |
| `page`, `perPage` | 1, 50 | Paging by number; `perPage` is 1 to 50. |
| `cursor` | none | Paging by cursor instead: `"*"` for the first page, then the answer's `next`. Steam's pages are always 50 items here. |

- `total` is how many items match, by Steam's count: after the ratings not asked for are left out by
  their tags, before the helper leaves out the rest.
- `next` is what to ask for to get the following page, or `null` after the last: a page number when
  paging by number, a cursor string when paging by cursor.
- `hidden` is how many items of this page the helper left out for their rating: items without a
  rating tag, and items with a content descriptor, when `Mature` is not asked for. A page can then
  hold fewer than `perPage` items. Steam has no query for a missing tag or for descriptors, so the
  helper filters each page.
- Steam's pages hold 50 items; a page of another size may take two of Steam's, which the helper asks
  for in turn. Sorting the same query twice can differ slightly as votes come in.

### details

```
→ {"id":3,"op":"details","itemId":"3100000001"}
← {"id":3,"ok":true,"item":{...,"visibility":"public","previews":[{"url":"...","type":"image","name":"street.jpg"}]}}
```

`not_found` when there is no such item or it is not visible to this account (with Steam's `result`).

### subscribe, unsubscribe

```
→ {"id":4,"op":"subscribe","itemId":"3100000007"}
← {"id":4,"ok":true,"result":1}
```

`result` is Steam's `EResult`, 1 when it worked; a failure is `steam_error`. Steam then downloads a
subscribed item by itself (`installed` follows) and removes an unsubscribed one's files later, when
AnimeLogon is no longer running. `subscriptionsChanged` follows either.

### subscribed

What this account subscribes to, with each item's state on this computer:

```
→ {"id":5,"op":"subscribed"}
← {"id":5,"ok":true,"details":true,"items":[
     {"id":"3100000001",
      "state":{"subscribed":true,"installed":true,"needsUpdate":false,"downloading":false,"downloadPending":false},
      "folder":"D:\\Steam\\steamapps\\workshop\\content\\<appid>\\3100000001","sizeOnDisk":182000000,"timeStamp":1762000000,
      "title":"夜樱 Night Sakura","timeUpdated":1762000000,"preview":"https://...","descriptors":[],"rating":"Everyone"}]}
```

- `folder`, `sizeOnDisk` and `timeStamp` (when the files on disk were updated) are there only for an
  item with files on this computer.
- `title`, `timeUpdated`, `preview`, `descriptors` and `rating` come from a query of Steam's servers, 50 items
  at a time. When that query fails (offline, say), the answer still comes, with `"details": false`
  and those members missing.
- An item whose `timeUpdated` is later than the one the settings app imported has an update.

### download

```
→ {"id":6,"op":"download","itemId":"3100000007","highPriority":true}
← {"id":6,"ok":true}
```

Starts or hurries the download of an item's files; the answer only says that it started. `progress`
events follow while it downloads, then `installed`, or `downloadFailed`. A download that is already
complete gives `installed` at once. `highPriority` (default false) puts it before Steam's other
downloads. Downloading does not subscribe.

### publish

Creates a Workshop item, or updates one, and uploads a folder to it.

```
→ {"id":7,"op":"publish","folder":"C:\\Users\\me\\AppData\\Local\\Temp\\export","preview":"C:\\...\\preview.jpg",
   "title":"My theme","description":"...","visibility":"public","tags":["Video","4K","Mature"],
   "descriptors":["violenceGore"],"changeNote":"First version"}
← {"event":"progress","op":"publish","request":7,"itemId":"3100000100","stage":"uploadingContent","done":271000,"total":1000000}
← {"id":7,"ok":true,"itemId":"3100000100","created":true,"needsLegalAgreement":false}
```

| Parameter | |
|---|---|
| `itemId` | The item to update; without it a new item is created. |
| `folder` | Required. Full path of the folder whose files become the item's content. |
| `preview` | Full path of the preview picture. Steam limits its size: a JPEG under 1 MB is safe. |
| `title` | Required for a new item; 1 to 128 bytes of UTF-8. |
| `description` | Up to 7999 bytes of UTF-8. |
| `visibility` | A visibility name. |
| `tags` | Every tag of the item: printable, no comma, at most 255 bytes each. The settings app sends the tags of "Tags", one rating among them. |
| `descriptors` | Every content descriptor of the item, by name. On an update, those not listed are taken off. |
| `changeNote` | Shown in the item's change notes. |

- A parameter left out leaves that field as it is on an existing item.
- `itemId` is the item, `created` whether this request created it.
- `needsLegalAgreement`: the person has not accepted the Steam Workshop legal agreement; the item stays
  hidden until they do. Send them to
  `https://steamcommunity.com/sharedfiles/workshoplegalagreement`.
- A failure after the item was created (the upload failed) still names `itemId` and `created`, so the
  settings app can record the item and retry the upload on it instead of creating another.
- `progress` events (`op: "publish"`, `request` = this request's id) come while Steam prepares and
  uploads; there is no time limit on the answer.

### quit

`{"id":8,"op":"quit"}` is answered `{"id":8,"ok":true}`; then the helper disconnects from Steam and exits.

## Events

| Event | Members | When |
|---|---|---|
| `installed` | `id`, and `folder`, `sizeOnDisk`, `timeStamp` when Steam reports them | An item of this app was downloaded and installed (after `download`, or a subscription, or an update). Sent once per installation. |
| `downloadFailed` | `id`, `result`, `message` | A download started with `download` failed. |
| `progress` | `op: "download"`, `id`, `done`, `total` (bytes) | Every quarter second while a download started with `download` goes on, when it moved. |
| `progress` | `op: "publish"`, `request`, `itemId`, `stage`, `done`, `total` | Every quarter second while an upload goes on, when it moved. `done`/`total` count bytes in `uploadingContent` and `uploadingPreview`, else 0. |
| `subscriptionsChanged` | | The account's subscriptions to this app's items changed (here or anywhere else). |
| `status` | `steam`, `message` | Steam was closed while the helper was connected (`steam` is `not_running`). Everything waiting fails with `not_running`. |

## Developing without Steam

The x64 build also makes a stand-in for Steam's library, `build\x64\fake-steam\Release\steam_api64.dll`
(target `fake_steam_api`, source `tests/fake_steam/`). It exports every entry point the helper uses
and answers from a JSON fixture. It is built into a folder of its own so that it is never copied
beside the real programs; never ship it.

To run the settings app's Workshop pages against it:

1. Put the helper and the stand-in in one folder, the way the Steam edition installs them beside the
   settings app (`<install>\steam\`):
   ```powershell
   $bin = 'build\x64\bin\Release'
   New-Item -ItemType Directory -Force "$bin\steam" | Out-Null
   Copy-Item "$bin\animelogon-steam.exe" "$bin\steam\"
   Copy-Item 'build\x64\fake-steam\Release\steam_api64.dll' "$bin\steam\"
   ```
2. Copy `steam/dev/fixture.json` somewhere outside the repository (for example `D:\tmp\steam-dev\`).
   Installed items' folders are relative to the fixture (`content\<id>`); put a `.altheme` in each to
   try importing them.
3. Set `ANIMELOGON_FAKE_STEAM` to the fixture's full path in the environment the settings app starts
   in; the helper inherits it. Optionally set `ANIMELOGON_FAKE_STEAM_LOG` to a file that gets one line
   per notable call (init, shutdown, download, create, every field of an update, submit).
4. Start the helper with any app id but 0, e.g. `--appid 480`. The settings app starts it with the
   app id in `ANIMELOGON_STEAM_APPID` when that is set, so set it to 480 as well.

While `ANIMELOGON_FAKE_STEAM` is set, the settings app also serves the fixture's folder as
`https://steam-fixture.animelogon.invalid/`, which its page may show pictures from: a fixture item's
`preview` can name a picture beside the fixture, as `steam/dev/fixture.json` does
(`previews\<id>.jpg`; put pictures there, or the page shows a plain frame).

The stand-in reads the fixture when the helper connects. Subscribing, downloading and publishing
change its items in memory only.

### Fixture

```json
{
  "running": true,          // false: Steam is not running
  "initResult": 0,          // anything else: connecting fails with that ESteamAPIInitResult (3: version mismatch)
  "loggedOn": true,
  "owns": true,             // false: the account does not own the app
  "needsLegalAgreement": false,
  "uploadMs": 600,          // how long an upload takes
  "downloadMs": 400,        // how long a download takes
  "nextItemId": "4000000000",  // the id of the next item created
  "fail": {"query": 2, "details": 2, "subscribe": 2, "unsubscribe": 2, "download": 54, "create": 25, "submit": 2},
  "items": [{
    "id": "3100000001", "title": "...", "description": "...", "owner": "76561198000000101",
    "preview": "https://...", "tags": ["Video"], "descriptors": [1, "violenceGore"],
    "votesUp": 0, "votesDown": 0, "score": 0.9, "subscriptions": 0, "favorites": 0, "fileSize": 0,
    "trend": 0, "timeCreated": 0, "timeUpdated": 0, "visibility": 0,
    "subscribed": false, "installed": false, "needsUpdate": false,
    "folder": "content\\3100000001", "sizeOnDisk": 0,
    "previews": [{"url": "...", "type": 0, "name": "..."}]
  }]
}
```

(The comments are for this page only: the fixture is strict JSON.)

- `fail` makes an operation fail with that `EResult`: Steam's answer to the query, the details query,
  subscribing, unsubscribing, creating an item or submitting an upload, or the end of a download.
- Rankings: `trend` sorts by the item's `trend` (default: its `subscriptions`), `recent` by
  `timeCreated`, `subscribed` by `subscriptions`, `rating` by `score`. Pages hold 50 items; cursors
  are `o<offset>`. `text` matches titles, descriptions and tags without regard to ASCII case, and so
  do required and excluded tags.
- `descriptors` are numbers (1 to 5) or the names above. `folder` is relative to the fixture's
  folder unless it is a full path; an installed item without one is in `content\<id>` there.
- A publish applies the update's fields to the item when the upload ends, and fails with
  `FileNotFound` when the content folder does not exist.
- Tags in `steam/dev/fixture.json` are from the list above (see Tags).

## Checking against the SDK

Everything in `src/steam_flat.h` comes from Valve's public documentation, the MIT-licensed
Steamworks.NET bindings and memory, not from the SDK. When there is an SDK, compare locally (not in
CI, and without adding any of its files here):

- each function's flat name and parameters against `steam_api_flat.h`, especially
  `SteamAPI_InitFlat`, `SteamInternal_SteamAPI_Init`, the `SteamAPI_ManualDispatch_*` functions and
  the trailing `bool` of `GetNumSubscribedItems`, `GetSubscribedItems` and `SetItemTags`;
- the versioned accessors the SDK exports (`SteamAPI_SteamUGC_v0NN`, `SteamAPI_SteamUser_v0NN`,
  `SteamAPI_SteamApps_v0NN`) against the lists in `steam_flat.h`;
- every structure's members, array lengths and size under `#pragma pack(8)` against `isteamugc.h`,
  `isteamremotestorage.h`, `steam_api.h` and `isteamutils.h` (`SteamUGCDetails_t`,
  `SteamUGCQueryCompleted_t`, `CreateItemResult_t`, `SubmitItemUpdateResult_t`,
  `DownloadItemResult_t`, `ItemInstalled_t`, `RemoteStorage(Un)subscribePublishedFileResult_t`,
  `SteamAPICallCompleted_t`, `CallbackMsg_t`, `SteamParamStringArray_t`) and each `k_iCallback`;
- that `SteamAPI_ManualDispatch_Init` belongs after `SteamAPI_Init`, and that connecting again after
  `SteamAPI_Shutdown` (when Steam was closed and started again) works;
- that setting `SteamAppId` is enough for a program Steam did not start, or whether
  `SteamAPI_RestartAppIfNecessary` is needed;
- the hosts of the preview URLs Steam gives (`preview` and `previews`), which the settings page's
  Content-Security-Policy must allow (`app/web/index.html`): it allows
  `https://images.steamusercontent.com` and `https://steamuserimages-a.akamaihd.net`.
