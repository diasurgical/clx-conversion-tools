#pragma once

#include <cstddef>
#include <cstdint>

namespace dvl_gfx {

struct PCXHeader {
	uint8_t manufacturer;
	uint8_t version;
	uint8_t encoding;
	uint8_t bitsPerPixel;
	uint16_t xmin;
	uint16_t ymin;
	uint16_t xmax;
	uint16_t ymax;
	uint16_t hDpi;
	uint16_t vDpi;
	uint8_t colormap[48];
	uint8_t reserved;
	uint8_t nPlanes;
	uint16_t bytesPerLine;
	uint16_t paletteInfo;
	uint16_t hscreenSize;
	uint16_t vscreenSize;
	uint8_t filler[54];
};

inline constexpr size_t PcxHeaderSize = 128;

/**
 * @brief Reads the PCX header.
 *
 * @param data PCX data, at least `PcxHeaderSize` bytes.
 * @param bytesPerLine The size of each decoded scanline (per plane), including padding.
 * @return Pointer to the start of the pixel data.
 */
const uint8_t *LoadPcxMeta(const uint8_t *data, int &width, int &height, uint8_t &bpp,
    uint8_t &numPlanes, unsigned &bytesPerLine);

} // namespace dvl_gfx
