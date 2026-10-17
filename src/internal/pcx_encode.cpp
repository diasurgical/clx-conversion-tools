#include <pcx_encode.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <fstream>
#include <memory>
#include <optional>
#include <span>

#include <dvl_gfx_common.hpp>
#include <dvl_gfx_endian.hpp>
#include <pcx.hpp>

namespace dvl_gfx {
namespace {

/**
 * @brief The PCX specification requires the number of bytes per scanline to be even.
 */
uint16_t PcxBytesPerLine(int16_t width)
{
	return static_cast<uint16_t>(width + (width % 2));
}

/**
 * @brief Write the PCX-file header
 * @param width Image width
 * @param height Image height
 * @param out File stream to write to
 * @return True on success
 */
bool CaptureHdr(int16_t width, int16_t height, std::ostream *out)
{
	PCXHeader buffer;
	memset(&buffer, 0, sizeof(buffer));
	buffer.manufacturer = 10;
	buffer.version = 5;
	buffer.encoding = 1;
	buffer.bitsPerPixel = 8;
	buffer.xmax = SwapLE(width - 1);
	buffer.ymax = SwapLE(height - 1);
	buffer.hDpi = SwapLE(width);
	buffer.vDpi = SwapLE(height);
	buffer.nPlanes = 1;
	buffer.bytesPerLine = SwapLE(PcxBytesPerLine(width));
	out->write(reinterpret_cast<const char *>(&buffer), sizeof(buffer));
	return !out->fail();
}

/**
 * @brief Write the current in-game palette to the PCX file
 * @param palette Current palette
 * @param out File stream for the PCX file.
 * @return True if successful, else false
 */
bool CapturePal(std::span<const uint8_t> palette, std::ostream *out)
{
	uint8_t pcxPalette[1 + 256 * 3];
	pcxPalette[0] = 12;
	std::memcpy(&pcxPalette[1], palette.data(), 256 * 3);
	out->write(reinterpret_cast<const char *>(pcxPalette), sizeof(pcxPalette));
	return !out->fail();
}

/**
 * @brief RLE compress the pixel data
 * @param src Raw pixel buffer
 * @param dst Output buffer
 * @param width Width of pixel buffer

 * @return Output buffer
 */
uint8_t *CaptureEnc(const uint8_t *src, uint8_t *dst, int width)
{
	int rleLength;

	do {
		uint8_t rlePixel = *src;
		src++;
		rleLength = 1;

		width--;

		while (width > 0 && rleLength < 63 && rlePixel == *src) {
			rleLength++;

			width--;
			src++;
		}

		if (rleLength > 1 || rlePixel > 0xBF) {
			*dst = rleLength | 0xC0;
			dst++;
		}

		*dst = rlePixel;
		dst++;
	} while (width > 0);

	return dst;
}

/**
 * @brief Write the pixel data to the PCX file
 *
 * @param buf Pixel data
 * @param out File stream for the PCX file.
 * @return True if successful, else false
 */
bool CapturePix(std::span<const uint8_t> pixels, Size size, uint32_t pitch, std::ostream *out)
{
	const int width = size.width;
	const int bytesPerLine = PcxBytesPerLine(width);
	std::unique_ptr<uint8_t[]> lineBuffer { new uint8_t[bytesPerLine] };
	std::memset(lineBuffer.get(), 0, bytesPerLine);
	std::unique_ptr<uint8_t[]> pBuffer { new uint8_t[2 * bytesPerLine] };
	const uint8_t *lineBegin = pixels.data();
	for (int height = size.height; height > 0; height--) {
		std::memcpy(lineBuffer.get(), lineBegin, width);
		const uint8_t *pBufferEnd = CaptureEnc(lineBuffer.get(), pBuffer.get(), bytesPerLine);
		lineBegin += pitch;
		out->write(reinterpret_cast<const char *>(pBuffer.get()), pBufferEnd - pBuffer.get());
		if (out->fail())
			return false;
	}
	return true;
}

} // namespace

std::optional<IoError> PcxEncode(
    std::span<const uint8_t> pixels, Size size, uint32_t pitch,
    std::span<const uint8_t> palette, std::ostream *out)
{
	bool success = CaptureHdr(size.width, size.height, out);
	if (success) {
		success = CapturePix(pixels, size, pitch, out);
	}
	if (success) {
		success = CapturePal(palette, out);
	}
	if (!success) {
		return IoError { std::string("Failed when writing PCX file: ").append(std::strerror(errno)) };
	}
	return std::nullopt;
}

} // namespace dvl_gfx
