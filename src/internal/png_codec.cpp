#include "png_codec.hpp"

#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <optional>
#include <span>
#include <string>
#include <vector>

#include <png.h>

#include <dvl_gfx_common.hpp>

// libpng reports errors via longjmp.
// The functions that call `setjmp` below must not have any local variables
// with non-trivial destructors, as longjmp would skip them.

namespace dvl_gfx {
namespace {

struct PngErrorState {
	char message[256];
};

[[noreturn]] void PngErrorFn(png_structp png, png_const_charp message)
{
	auto *state = static_cast<PngErrorState *>(png_get_error_ptr(png));
	std::snprintf(state->message, sizeof(state->message), "%s", message);
	png_longjmp(png, 1);
}

void PngWarningFn(png_structp /*png*/, png_const_charp /*message*/)
{
}

struct PngReadState {
	const uint8_t *data;
	size_t size;
	size_t pos;
};

void PngReadFn(png_structp png, png_bytep out, size_t length)
{
	auto *state = static_cast<PngReadState *>(png_get_io_ptr(png));
	if (length > state->size - state->pos)
		png_error(png, "unexpected end of data");
	std::memcpy(out, state->data + state->pos, length);
	state->pos += length;
}

void PngWriteFn(png_structp png, png_bytep data, size_t length)
{
	auto *out = static_cast<std::vector<uint8_t> *>(png_get_io_ptr(png));
	out->insert(out->end(), data, data + length);
}

void PngFlushFn(png_structp /*png*/)
{
}

bool DecodePng(png_structp png, png_infop info, PngReadState *readState, PalettedImage *out)
{
	if (setjmp(png_jmpbuf(png)))
		return false;

	png_set_read_fn(png, readState, PngReadFn);
	png_read_info(png, info);

	png_uint_32 width;
	png_uint_32 height;
	int bitDepth;
	int colorType;
	png_get_IHDR(png, info, &width, &height, &bitDepth, &colorType, nullptr, nullptr, nullptr);
	if (colorType != PNG_COLOR_TYPE_PALETTE)
		png_error(png, "image is not palettized (indexed color)");

	png_colorp plte;
	int numPalette;
	if (png_get_PLTE(png, info, &plte, &numPalette) == 0)
		png_error(png, "image has no palette");
	out->palette.fill(0);
	for (int i = 0; i < numPalette && i < 256; ++i) {
		out->palette[i * 3] = plte[i].red;
		out->palette[i * 3 + 1] = plte[i].green;
		out->palette[i * 3 + 2] = plte[i].blue;
	}

	out->transparentColor = std::nullopt;
	png_bytep trans;
	int numTrans;
	if (png_get_tRNS(png, info, &trans, &numTrans, nullptr) != 0) {
		for (int i = 0; i < numTrans && i < 256; ++i) {
			if (trans[i] == 0) {
				out->transparentColor = static_cast<uint8_t>(i);
				break;
			}
		}
	}

	if (bitDepth < 8)
		png_set_packing(png);
	const int numPasses = png_set_interlace_handling(png);
	png_read_update_info(png, info);

	out->size = Size { width, height };
	out->pixels.resize(static_cast<size_t>(width) * height);
	for (int pass = 0; pass < numPasses; ++pass) {
		for (png_uint_32 y = 0; y < height; ++y) {
			png_read_row(png, &out->pixels[static_cast<size_t>(y) * width], nullptr);
		}
	}
	png_read_end(png, nullptr);
	return true;
}

bool EncodePng(png_structp png, png_infop info,
    const uint8_t *pixels, Size size, uint32_t pitch,
    const uint8_t *palette, const int *transparentColor,
    std::vector<uint8_t> *out)
{
	if (setjmp(png_jmpbuf(png)))
		return false;

	png_set_write_fn(png, out, PngWriteFn, PngFlushFn);
	png_set_IHDR(png, info, size.width, size.height, 8, PNG_COLOR_TYPE_PALETTE,
	    PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

	png_color plte[256];
	for (unsigned i = 0; i < 256; ++i) {
		plte[i].red = palette[i * 3];
		plte[i].green = palette[i * 3 + 1];
		plte[i].blue = palette[i * 3 + 2];
	}
	png_set_PLTE(png, info, plte, 256);

	if (transparentColor != nullptr) {
		png_byte trans[256];
		std::memset(trans, 0xFF, sizeof(trans));
		trans[*transparentColor] = 0;
		png_set_tRNS(png, info, trans, *transparentColor + 1, nullptr);
	}

	png_set_compression_level(png, 9);
	png_write_info(png, info);
	for (uint32_t y = 0; y < size.height; ++y) {
		png_write_row(png, pixels + static_cast<size_t>(y) * pitch);
	}
	png_write_end(png, info);
	return true;
}

} // namespace

std::optional<IoError> PngDecodePaletted(std::span<const uint8_t> data, PalettedImage &out)
{
	constexpr size_t PngSignatureSize = 8;
	if (data.size() < PngSignatureSize || png_sig_cmp(data.data(), 0, PngSignatureSize) != 0)
		return IoError { "Not a PNG file" };

	PngErrorState errorState {};
	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &errorState, PngErrorFn, PngWarningFn);
	if (png == nullptr)
		return IoError { "Failed to initialize libpng" };
	png_infop info = png_create_info_struct(png);
	if (info == nullptr) {
		png_destroy_read_struct(&png, nullptr, nullptr);
		return IoError { "Failed to initialize libpng" };
	}

	PngReadState readState { data.data(), data.size(), 0 };
	const bool success = DecodePng(png, info, &readState, &out);
	png_destroy_read_struct(&png, &info, nullptr);
	if (!success)
		return IoError { std::string("Failed to decode PNG: ").append(errorState.message) };
	return std::nullopt;
}

std::optional<IoError> PngEncodePaletted(
    std::span<const uint8_t> pixels, Size size, uint32_t pitch,
    std::span<const uint8_t> palette, std::optional<uint8_t> transparentColor,
    std::vector<uint8_t> &out)
{
	if (palette.size() < 256 * 3)
		return IoError { "Palette must have 256 entries" };
	if (size.height != 0 && pixels.size() < static_cast<size_t>(size.height - 1) * pitch + size.width)
		return IoError { "Pixel buffer is too small" };

	PngErrorState errorState {};
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, &errorState, PngErrorFn, PngWarningFn);
	if (png == nullptr)
		return IoError { "Failed to initialize libpng" };
	png_infop info = png_create_info_struct(png);
	if (info == nullptr) {
		png_destroy_write_struct(&png, nullptr);
		return IoError { "Failed to initialize libpng" };
	}

	const int transparentColorInt = transparentColor.value_or(0);
	const bool success = EncodePng(png, info, pixels.data(), size, pitch, palette.data(),
	    transparentColor.has_value() ? &transparentColorInt : nullptr, &out);
	png_destroy_write_struct(&png, &info);
	if (!success)
		return IoError { std::string("Failed to encode PNG: ").append(errorState.message) };
	return std::nullopt;
}

} // namespace dvl_gfx
