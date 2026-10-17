# Bloodframe

Bloodframe runs **Lossless Scaling Frame Generation 3.1** on Android, on top of Vulkan. It is a single
shared library, `libbloodframe.so`, that a native Android (Bionic) process loads to turn the frames it
renders into more frames (2x, 3x, 4x): the host hands it each real frame as an `AHardwareBuffer`, and gets
generated frames back as `AHardwareBuffer`s with sync files, so the host keeps its own presentation.

- **Your own Lossless.dll.** The frame generation shaders are read, at run time, from the `Lossless.dll` of
  your own copy of Lossless Scaling (Steam, app 993090). The path is given by the host
  (`BbLsfgConfig.dll_path`). **No shaders and no `Lossless.dll` are included in this repository or in its
  builds.**
- **Half precision.** When the `Lossless.dll` carries the half-precision (FP16) shader set and the GPU has
  `shaderFloat16`, Bloodframe runs those shaders (faster on mobile GPUs). If either is missing, or the
  driver rejects a half-precision shader, it falls back to the full-precision (FP32) shaders for the rest
  of the process. `bb_lsfg_precision()` says what runs and why.
- **Same driver as the host.** Bloodframe makes its own `VkInstance` and `VkDevice` through the host's own
  `vkGetInstanceProcAddr`, on the host's physical device, so it runs on the very driver the process loaded
  (for example a user-supplied Turnip).
- **C API.** `android/bb_lsfg.h` is the whole interface: `bb_lsfg_init`, `bb_lsfg_create_context`,
  `bb_lsfg_present`, `bb_lsfg_wait`, `bb_lsfg_destroy_context`, `bb_lsfg_shutdown`, `bb_lsfg_precision`,
  `bb_lsfg_diagnostics`. Only these `bb_lsfg_*` symbols are exported (`android/bb_lsfg.map`). Nothing
  throws; failures return -1 and write a message into the `err` buffer.

Bloodframe is not a Vulkan layer and has no config file: the host decides everything through the C API.

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

- `android/`: the C API (`bb_lsfg.cpp`, `bb_lsfg.h`), the half-precision SPIR-V set (`bb_half.cpp`) and the
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
