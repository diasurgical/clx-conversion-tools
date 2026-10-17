# DevilutionX Graphics Tools

## Build & install

```bash
cmake -S. -Bbuild-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
cmake --build build-rel -j $(getconf _NPROCESSORS_ONLN)
sudo cmake --install build-rel --component Binaries
```

The above only install the binaries. If you want to install the libraries and headers as well, run:

```bash
sudo cmake --install build-rel
```

### Build options

| Option | Default | Description |
|--------|---------|-------------|
| `ENABLE_PNG_TOOLS` | `ON` | Build `png2clx` and `clx2png`. |
| `USE_SYSTEM_LIBPNG` | `OFF` | Use the system libpng. By default, libpng is fetched and linked statically. |
| `USE_SYSTEM_ZLIB` | `OFF` | Use the system zlib for the fetched libpng. By default, zlib is fetched and linked statically. Ignored if `USE_SYSTEM_LIBPNG` is `ON`. |

## cel2clx

Converts CEL files to CLX. Run `cel2clx --help` for more information.

## cl22clx

Converts CL2 files to CLX. Run `cl22clx --help` for more information.

## clx2pcx

Converts CLX files to PCX. Run `clx2pcx --help` for more information.

## pcx2clx

Converts PCX files to CLX. Run `pcx2clx --help` for more information.

## png2clx

Converts palettized PNG files to CLX. Run `png2clx --help` for more information.

Within a PNG, sprites are stacked vertically (`--num-sprites`).
CLX sheets, such as player and monster graphics with one sprite list per direction,
can be created in two ways:

```bash
# A single PNG with 8 equal-width lists side by side (the layout produced by clx2png):
png2clx --num-lists 8 --num-sprites 10 wlsas.png

# One PNG per list, combined into wlsas.clx:
png2clx --combine --num-sprites 10 wlsas0.png wlsas1.png wlsas2.png wlsas3.png \
  wlsas4.png wlsas5.png wlsas6.png wlsas7.png
```

## clx2png

Converts CLX files to palettized PNG. Run `clx2png --help` for more information.
