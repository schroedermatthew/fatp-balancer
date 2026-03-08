# fatp-balancer WASM Demo Build

Builds the C++ balancer into a WebAssembly module that the `demo/index.html`
page loads automatically when present. Without the WASM files the demo falls
back to its pure-JavaScript simulation.

---

## Prerequisites

### Emscripten (one-time install)

```powershell
# From wherever you want emsdk to live, e.g. C:\tools
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
.\emsdk install latest
.\emsdk activate latest
.\emsdk_env.bat
```

`emsdk_env.bat` puts `emcmake` and `em++` on your PATH for that terminal
session. Run it again in any new terminal before building.

---

## Build

### Option A — build script (recommended)

From the **fatp-balancer root**:

```powershell
.\wasm\build-wasm.ps1
```

Or from inside `wasm/` directly:

```powershell
.\build-wasm.ps1
```

The script activates Emscripten automatically, runs CMake, builds, and
verifies the outputs landed in `demo/`. No manual steps required.

```powershell
# Override FAT-P path if yours differs from the default
.\wasm\build-wasm.ps1 -FatpInclude "C:\some\other\path\fat_p"

# Override emsdk location
.\wasm\build-wasm.ps1 -EmsdkDir "C:\tools\emsdk"

# Build and immediately start the demo server
.\wasm\build-wasm.ps1 -Serve
```

### Option B — manual steps

From the **fatp-balancer root**:

```powershell
mkdir build-wasm
cd build-wasm

emcmake cmake .\wasm `
    -DFATP_INCLUDE_DIR="C:\Users\mtthw\Desktop\AI Projects\FatP\include\fat_p" `
    -DCMAKE_BUILD_TYPE=Release

cmake --build . --config Release
```

The post-build step copies `balancer.js` and `balancer.wasm` directly into
`demo/`. No manual copy required.

---

## Run

Browsers block WASM from `file://` URLs — you must serve the files over HTTP:

```powershell
cd ..\demo
python -m http.server 8080
```

Then open `http://localhost:8080`. If the WASM backend loaded successfully
the event log will print:

```
⚡ WASM backend loaded — using compiled C++
```

If that line does not appear the demo is running on the JS simulation. Check
the browser console for load errors.

---

## Output files

| File | Location | Description |
|------|----------|-------------|
| `balancer.js` | `demo/` | Emscripten JS glue + module loader |
| `balancer.wasm` | `demo/` | Compiled C++ balancer |

Both files are gitignored build artefacts — rebuild from source after any
changes to the balancer headers or `BalancerBindings.cpp`.

---

## Troubleshooting

**`emcmake` not found** — run `emsdk_env.bat` in your current terminal.

**`Expected.h` not found** — check that `FATP_INCLUDE_DIR` points to the
directory containing `Expected.h`, not its parent.

**WASM not loading in browser** — confirm you are on `http://localhost:8080`
and not a `file://` URL. Check the browser console for a 404 on `balancer.js`.

**JS simulation still active after successful build** — verify `balancer.js`
and `balancer.wasm` are present in `demo/` alongside `index.html`.
