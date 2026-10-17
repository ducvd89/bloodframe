# Bloodframe

Bloodframe runs **Lossless Scaling Frame Generation 3.1** on Android, on top of Vulkan. It is a single
shared library, `libbloodframe.so`, that a native Android (Bionic) process loads to turn the frames it
renders into more frames (2x, 3x, 4x): the host hands it each real frame as an `AHardwareBuffer`, and gets
generated frames back as `AHardwareBuffer`s with sync files, so the host keeps its own presentation.

- **Your own Lossless.dll.** The frame generation shaders are read, at run time, from the `Lossless.dll` of
  your own copy of Lossless Scaling (Steam, app 993090). The path is given by the host
  (`BbLsfgConfig.dll_path`). **No shaders and no `Lossless.dll` are included in this repository or in its
  builds.**
- **Shader sets.** The `Lossless.dll` holds every shader three times: as DXBC, as SPIR-V for half
  precision (FP16) and as SPIR-V for full precision (FP32). Bloodframe runs the dll's own SPIR-V and
  translates DXBC only as the last resort. In order: **FP16 SPIR-V** (the host asked for it, the dll has the set
  and the GPU has `shaderFloat16`), then **FP32 SPIR-V**, then **DXBC (translated)** (an old dll without the
  SPIR-V sets, a module that fails the checks, or a driver that rejects a set at the first context). The
  fallback holds for the rest of the process. `bb_lsfg_shader_set()` says which set runs and why (and the
  library logs it once: `Bloodframe: shaders FP32 SPIR-V (...)`); `bb_lsfg_precision()` says half or full.
- **Same driver as the host.** Bloodframe makes its own `VkInstance` and `VkDevice` through the host's own
  `vkGetInstanceProcAddr`, on the host's physical device, so it runs on the very driver the process loaded
  (for example a user-supplied Turnip).
- **C API.** `android/bb_lsfg.h` is the whole interface: `bb_lsfg_init`, `bb_lsfg_create_context`,
  `bb_lsfg_present`, `bb_lsfg_wait`, `bb_lsfg_destroy_context`, `bb_lsfg_shutdown`, `bb_lsfg_shader_set`,
  `bb_lsfg_precision`, `bb_lsfg_diagnostics`. Only these `bb_lsfg_*` symbols are exported (`android/bb_lsfg.map`). Nothing
  throws; failures return -1 and write a message into the `err` buffer.

Bloodframe is not a Vulkan layer and has no config file: the host decides everything through the C API.

## Tested on

- **AYN Thor** (Snapdragon 8 Gen 2, Adreno 740, Turnip 26.3.0): FP16 runs; 2x at about 34 real + 34 generated
  frames/s in Bloodborne's Central Yharnam.
- **Oppo Find N5** (Snapdragon 8 Elite, Adreno 830, Turnip Gen8): FP16 runs, and it fixed a ghosting bug that the
  previous engine (the fork's DXBC translation path) had on this GPU: 10-40% of generated frames showed the picture
  doubled and shifted. With FP16 off, which then ran the translated DXBC, the ghosting came back (confirmed
  2026-10-17), so the translation was the cause. Since then full precision also runs the dll's own FP32 SPIR-V
  and translates DXBC only as the last resort: tested on the N5 with FP16 off (2026-10-17), no ghosting. Both
  precisions are clean there now.

## Credits and licence

Bloodframe is based on **lsfg-vk 1.0 by PancakeTAS** (MIT), via **GameNative's Android fork by Ragnarok93**
([lsfg-vk-android](https://github.com/Ragnarok93/lsfg-vk-android), MIT). This repository continues that
fork's history: the commits up to `0802cd3` are theirs, the commits after it are Bloodframe's. The
fork's original README is kept as `README.lsfg-vk-android.md`.

`LICENSE.md` is the fork's MIT licence, with a copyright line added for Bloodframe's changes (also MIT).
Linked into `libbloodframe.so`: [dxbc](https://github.com/PancakeTAS/dxbc) (PancakeTAS),
[pe-parse](https://github.com/trailofbits/pe-parse) (Trail of Bits) and [volk](https://github.com/zeux/volk)
(Arseny Kapoulkine), each under its own licence; `build-android.sh` copies those licences next to the
library (`out/licenses/`) and they must ship with it.

Bloodframe is **not affiliated with Lossless Scaling** (THS) **or with lsfg-vk**. "Lossless Scaling" is the
name of THS's product, used here only to say which frame generation the library is compatible with.

## What was changed from the Android fork

- `android/`: the C API (`bb_lsfg.cpp`, `bb_lsfg.h`), the SPIR-V shader sets of the dll (`bb_spirv.cpp`: half and full precision) and the
  CMake target `bloodframe`, which builds the framegen backend, the `Lossless.dll` shader extraction and
  dxbc/pe-parse/volk into one library. The fork's Vulkan layer, toml11 and UI are not built.
- `framegen/`: the Vulkan instance can come from the host's `vkGetInstanceProcAddr`; SPIR-V 1.0 modules are
  taken unchanged; `shaderFloat16` is enabled only when the host asks for it.
- `src/extract/`: the `Lossless.dll` is read from a path the host sets, and the extractor hands out the
  dll's resources and shader names.

## Building

Needs the Android NDK (r27 or newer; r29 is what Bloodframe is built with), `git`, `cmake` >= 3.18 and
`ninja`. The build is an arm64-v8a cross build:

    git clone https://github.com/ducvd89/bloodframe.git
    cd bloodframe
    ./build-android.sh --ndk /path/to/android-ndk --api 30 --out out

Arguments: `--ndk PATH` (default: `$ANDROID_NDK_HOME`, `$ANDROID_NDK_ROOT` or the newest NDK under
`$ANDROID_HOME/ndk`), `--api N` (default 30), `--out DIR` (default `./out`), `--build-dir DIR`, `--jobs N`.

Results: `out/lib/libbloodframe.so` (stripped of debug info, `c++_shared` STL, so ship `libc++_shared.so`
from the NDK with it) and the licences in `out/licenses/`.

With CMake directly: `cmake -S . -B build -G Ninja -DBLOODFRAME=ON -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release`
and build the target `bloodframe`. (The submodules `thirdparty/dxbc`, `pe-parse` and `volk` must be checked
out; `build-android.sh` does that.)
