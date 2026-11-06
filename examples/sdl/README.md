# SDL graphics in Aether

Aether lowers to the PSCAL VM, which ships a full SDL backend, spelled in
lowercase as every builtin is (D70): `initgraph`,
`setrgbcolor`, `fillcircle` / `fillrect` / `drawcircle`, `updatescreen`,
`graphloop`, `pollkey` / `iskeydown` / `getmousestate`, `outtextxy`,
`loadimagetotexture`, `rendercopy`, and more. The default aether build is minimal
and leaves SDL out, so it is behind an opt-in CMake flag.

## Build with SDL

Needs the SDL2 development libraries (core + image + ttf + mixer) and OpenGL.
On macOS: `brew install sdl2 sdl2_image sdl2_ttf sdl2_mixer`.

```sh
cmake -S . -B build-sdl -DAETHER_ENABLE_SDL=ON
cmake --build build-sdl -j
./build-sdl/aether --no-cache examples/sdl/circles.aether
```

`-DAETHER_ENABLE_SDL=ON` forces pscal-core's `PSCAL_SDL` option on, which compiles
the SDL / GL / audio backend and links SDL2 + OpenGL (plus the Metal / QuartzCore /
Foundation frameworks on macOS). The `SDL` define and the libraries are PUBLIC on
`pscal_core_static`, so aether inherits both.

## Calling SDL from Aether

The SDL routines are I/O effects, so call them inside an `fx { ... }` block.
Coordinates and sizes are `Int`; window titles and file paths are multi-character
`Text` literals (a single-character literal is a `Char`, which `initgraph`'s
`String` argument rejects). Builtins that return values through VAR parameters
(e.g. `getpixelcolor`) are not expressible from Aether yet.
