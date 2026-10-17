#pragma once

#include <cstdint>

#include <array>
#include <optional>
#include <span>
#include <vector>

#include <dvl_gfx_common.hpp>

namespace dvl_gfx {

struct PalettedImage {
	/** @brief 8-bit color-indexed pixels, `size.width` bytes per line. */
	std::vector<uint8_t> pixels;
	Size size;

	/** @brief RGB palette. Entries beyond those defined by the PNG are set to 0. */
	std::array<uint8_t, 256 * 3> palette;

	/** @brief The first fully transparent palette entry (from the tRNS chunk), if any. */
	std::optional<uint8_t> transparentColor;
};

/**
 * @brief Decodes a palettized (color type 3) PNG image.
 *
 * Images with bit depths lower than 8 are expanded to 1 byte per pixel.
 * Returns an error if the image is not palettized.
 */
std::optional<IoError> PngDecodePaletted(std::span<const uint8_t> data, PalettedImage &out);

/**
 * @brief Encodes an 8-bit color-indexed pixel buffer as a palettized (color type 3) PNG image.
 *
 * @param pixels The pixel buffer.
 * @param size Image dimensions.
 * @param pitch Pixel buffer pitch, i.e. the width of each line including padding.
 * @param palette RGB palette (256 * 3 bytes).
 * @param transparentColor If set, this palette entry is marked as fully transparent (tRNS).
 * @param out Output PNG buffer.
 */
std::optional<IoError> PngEncodePaletted(
    std::span<const uint8_t> pixels, Size size, uint32_t pitch,
    std::span<const uint8_t> palette, std::optional<uint8_t> transparentColor,
    std::vector<uint8_t> &out);

} // namespace dvl_gfx
