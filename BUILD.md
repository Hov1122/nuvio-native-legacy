# Building the Tizen package

This is the exact flow used to produce the `.wgt` that runs on Samsung TVs:
C sources → WebAssembly (Emscripten) → staged web app → unsigned `.wgt` →
sign in Tizen Studio → install on the TV. Every step is a script in `tools/`;
there are no manual file copies.

```bash
bash tools/tizen.sh     # compile  -> build/tizen/
bash tools/tizen-wgt.sh # package  -> NuvioTV-native.wgt (unsigned)
bash tools/testa-tudo.sh  # unit tests (run any time)
```

## 1. Prerequisites

On the build machine (WSL2/Ubuntu or any Linux):

| Need | Why | How |
|---|---|---|
| Emscripten SDK (upstream) | `emcc`, the WASM compiler | `git clone https://github.com/emscripten-core/emsdk ~/emsdk && cd ~/emsdk && ./emsdk install latest && ./emsdk activate latest` (override with `EMSDK_DIR`) |
| Python 3 + Pillow | converts the 41 webp badges to png (`tizen-art.sh` picks `dwebp`/`magick`/`ffmpeg` first if present) | `python3 -m pip install --user --break-system-packages Pillow` |
| Node.js + npx | downgrades the emcc JS glue to chrome76 for old TV browsers (auto-fetched, no install step) | any recent Node |
| zip **or** Python 3 | zips the `.wgt` (`tizen-wgt.sh` falls back to Python's `zipfile` when `zip` is missing) | usually already there |

On Windows: **Tizen Studio** (Certificate Manager + Device Manager) for signing
and installing. Signing cannot happen on the Linux side: distributor
certificates pin the TV's DUID, so the `.wgt` leaves the build **unsigned on
purpose** and gets signed where the certificate lives.

## 2. Configuration (`local.properties`)

The build bakes the server addresses in at compile time — changing them means
rebuilding. Copy the template and fill the blanks:

```bash
cp /path/to/NuvioWeb-0.3.38-beta/local.example.properties \
   ~/NuvioWeb-0.3.38-beta/local.properties
```

(`tools/env.sh` looks for `NuvioWeb-0.3.38-beta/local.properties` two
directories above the repo by default; override with `NUVIO_PROPERTIES`.)

| Key | Value | Notes |
|---|---|---|
| `NUVIO_SUPABASE_URL` | `https://api.nuvio.tv` | bare host — the app appends `/rest/v1/` and `/auth/v1/` itself. **No trailing path.** |
| `NUVIO_SUPABASE_ANON_KEY` | `sb_publishable_…` | publishable key: public by design, safe to ship in the package. Never the service-role secret. |
| `TV_LOGIN_WEB_BASE_URL` | `https://nuvio.tv/tv-login` | redirect base for the QR login flow; must contain `/tv-login` or the server rejects the session request. Empty = login screen errors with "no login address". |
| `TMDB_API_KEY`, `TRAKT_CLIENT_ID/SECRET`, `SIMKL_CLIENT_ID` | optional | account sync fills most of these in; local values are fallback. |
| `YOUTUBE_RELAY_URL` | `https://<you>.pages.dev/youtube-proxy.html` | optional; **required for trailers** (see §6). Full https URL of the file. |

If URL or anon key is empty, the build still succeeds but prints a warning —
and the TV shows "this package was built without a server". That message
always means the properties, never the network.

## 3. Build — `bash tools/tizen.sh`

Compiles `src/*.c` with emcc (`-O2 -pthread`, 256 MiB heap, IDBFS for
session persistence), converts artwork via `tizen-art.sh`, preloads it, and
writes `build/tizen/` (`index.html`, `index.js`, `index.wasm`, `index.data`).
Takes a few minutes. knobs:

- `NUVIO_SAIDA=dir` — stage somewhere else (default `build/tizen`).
- `NUVIO_YOUTUBE_RELAY_URL=https://…` — same as the properties key, but
  wins for one-off builds without editing the file.
- `NUVIO_LOG_URL=http://host:port/log` — makes the TV POST its log
  somewhere instead of depending on screenshots of the on-screen panel.

## 4. Package — `bash tools/tizen-wgt.sh`

Stages `build/tizen/` + `config.xml` + icon, refuses to proceed if any
expected file is missing or a credential-looking `.txt` slipped in, and
writes `NuvioTV-native.wgt` (override name with `NUVIO_WGT_NOME`). **Unsigned
by design** — see §1. Install flow from here is all Tizen Studio: open the
Certificate Manager, make sure the distributor certificate lists the TV's
DUID, sign the `.wgt`, install via Device Manager (`sdb`).

## 5. Verify on the TV

- Loading screen: name + spinner, no text. It hides on the first drawn frame;
  if it never hides, the red-button log panel (green key brings it back) says
  why — photograph it, that's the whole diagnostic.
- Login: QR code → approve on the phone → home. Anonymous auth happens
  first, so a working server shows movement even before approving.
- Trailers need §6, otherwise their row stays hidden (honest empty state).

Useful log lines (red-button panel): `[ajustes] idiomas da conta` (effective
audio/subtitle prefs), `[linguas] audio alvos=…` (per-playback track pick),
`[extras] trailers …` (what each trailer source found), `[tex] cache limpo`.

## 6. Trailers need a relay (YouTube 153)

YouTube rejects embeds from `file://` origins (error 153), and a `.wgt` page
*is* `file://` — no header, param, or player flag fixes that from inside the
package. The working setup is one static file on free HTTPS hosting:

1. `tools/youtube-proxy.html` in this repo (byte-identical to the web app's —
   **do not edit it**, or it stops diffing clean against upstream).
2. Upload just that file to any static host (e.g. Cloudflare Pages → Upload
   assets; a bare folder with the file gives
   `https://<name>.pages.dev/youtube-proxy.html`). Confirm in a browser: a
   black page means served, 404 means not yet.
3. Put the full file URL in `YOUTUBE_RELAY_URL` (properties or env, see §2–3)
   and rebuild. `tizen.sh` prints `trailer relay …` when it picks the URL up.

Cost is ~1 request per trailer opened (only the proxy html; video streams
from YouTube). Without the URL the app uses direct embeds, which 153 on
this firmware — the row then stays hidden instead of promising playback.

## 7. Tests — `bash tools/testa-tudo.sh`

Runs every `tests/*.sh` except the ones needing a screen, a server, or a
clock. Each case compiles its slice with the system `cc` and runs it; new
pure-C modules come with their own `tests/<name>.c` + `tests/<name>.sh`
following `tests/trailer.*` as the template. Known environment gaps (not
regressions — verified against a clean tree): anything needing macOS
frameworks (`player`, `posplay`, `proximo`), SDL2 headers (`webp`, `i18n`),
or live network (`conta`, `collections`, …).

## 8. Repo map

- `src/` — the app (C99; new code in English, existing Portuguese comments
  stay untouched). Platform split: `video.c` = webOS path, `video_tizen.c` =
  Tizen/AVPlay path, both behind `video.h`.
- `tools/` — build/packaging scripts above, plus the trailer relay file (§6).
  `tizen-shell.html` is the TV page hosting the WASM (loading screen, log
  panel, key translation, trailer overlay).
- `tests/` — one `.c` + `.sh` per unit.
- `deploy/app/art/` — bundled artwork (personal: never commit `trakt.txt`,
  `addons.txt`, or `collections/`).
- `build/` — generated, git-ignored. The `.wgt` at the repo root is a local
  artifact, not a release: sign before installing.
