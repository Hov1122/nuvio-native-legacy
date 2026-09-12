# nuvio-native-legacy

Nuvio is an open-source streaming app — movies, series, addons, Trakt/Simkl
sync, continue watching. This repo is a **native port for Samsung Tizen
TVs**: the app compiled to WebAssembly inside a `.wgt`, video through the
TV's AVPlay pipeline, with QR login, profiles and trailers.

Fork of [iqui27/nuvio-native-legacy](https://github.com/iqui27/nuvio-native-legacy),
which targets LG webOS.

## Build it yourself, step by step

**1. Install the tools** (on WSL2/Ubuntu or any Linux):

- Emscripten SDK: `git clone https://github.com/emscripten-core/emsdk ~/emsdk && cd ~/emsdk && ./emsdk install latest && ./emsdk activate latest`
- Python 3 + Pillow: `python3 -m pip install --user --break-system-packages Pillow`
- Node.js (any recent one, for the JS glue step)
- Tizen Studio on Windows (for signing and installing at the end)

**2. Point the build at a backend.** Copy the example properties next to the
repo as `NuvioWeb-0.3.38-beta/local.properties` and fill in:

```properties
NUVIO_SUPABASE_URL=https://api.nuvio.tv
NUVIO_SUPABASE_ANON_KEY=sb_publishable_…
TV_LOGIN_WEB_BASE_URL=https://nuvio.tv/tv-login
```

**3. Compile:**

```bash
bash tools/tizen.sh
```

This writes `build/tizen/` (`index.html`, `index.js`, `index.wasm`,
`index.data`). Takes a few minutes the first time.

**4. Package:**

```bash
bash tools/tizen-wgt.sh
```

This writes `NuvioTV-native.wgt`, intentionally **unsigned**.

**5. Sign and install.** In Tizen Studio's Certificate Manager, make sure the
distributor certificate lists your TV's DUID, sign the `.wgt`, and install it
from the Device Manager.

**6. Trailers (optional but expected).** YouTube rejects embeds from the
package as-is, so trailers play through one static relay file on free HTTPS
hosting — see the "Trailers need a relay" section of [BUILD.md](BUILD.md).
Without it the trailer rows simply stay hidden.

Run `bash tools/testa-tudo.sh` any time to check the unit tests.

## License

[GNU General Public License v3.0](LICENSE).
