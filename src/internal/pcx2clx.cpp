#include <pcx2clx.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include <dvl_gfx_endian.hpp>

#include "clx_encode.hpp"
#include "pcx.hpp"

namespace dvl_gfx {

std::optional<IoError> PcxToClx(const uint8_t *data, size_t size,
    int numFramesOrFrameHeight,
    std::optional<uint8_t> transparentColor,
    const std::vector<uint16_t> &cropWidths,
    std::vector<uint8_t> &clxData,
    uint8_t *paletteData)
{
	int width;
	int height;
	uint8_t bpp;
	uint8_t numPlanes;
	unsigned bytesPerLine;
	if (size < PcxHeaderSize) {
		return IoError { "data too small" };
	}
	const uint8_t *pixelData = LoadPcxMeta(data, width, height, bpp, numPlanes, bytesPerLine);
	const uint8_t *const dataEnd = data + size;
	const size_t pixelDataSize = size - PcxHeaderSize;
	if (bpp != 8 || numPlanes != 1)
		return IoError { "Only 8-bit single-plane PCX images are supported" };
	if (width <= 0 || height <= 0)
		return IoError { "Invalid PCX image dimensions" };
	if (bytesPerLine < static_cast<unsigned>(width))
		return IoError { "Invalid PCX header: bytes per line is less than the image width" };

	unsigned numFrames;
	unsigned frameHeight;
	if (numFramesOrFrameHeight > 0) {
		numFrames = numFramesOrFrameHeight;
		frameHeight = height / numFrames;
	} else if (numFramesOrFrameHeight < 0) {
		frameHeight = -numFramesOrFrameHeight;
		numFrames = height / frameHeight;
	} else {
		return IoError { "The number of frames must not be 0" };
	}
	if (numFrames == 0 || frameHeight == 0)
		return IoError { "Image is too small for the requested frame size or count" };
	for (const uint16_t cropWidth : cropWidths) {
		if (cropWidth > width)
			return IoError { "Crop width is larger than the image width" };
	}

	// CLX header: frame count, frame offset for each frame, file size
	clxData.reserve(pixelDataSize);
	clxData.resize(4 * (2 + static_cast<size_t>(numFrames)));
	WriteLE32(clxData.data(), numFrames);

	// We process the PCX a whole frame at a time because the lines are reversed
	// in CEL.
	auto frameBuffer = std::unique_ptr<uint8_t[]>(
	    new uint8_t[static_cast<size_t>(frameHeight) * width]);

	// Each scanline is `bytesPerLine` bytes long, which can be larger than the width
	// (the PCX specification requires `bytesPerLine` to be even).
	auto lineBuffer = std::unique_ptr<uint8_t[]>(new uint8_t[bytesPerLine]);

	const uint8_t *dataPtr = pixelData;
	for (unsigned frame = 1; frame <= numFrames; ++frame) {
		WriteLE32(&clxData[4 * static_cast<size_t>(frame)],
		    static_cast<uint32_t>(clxData.size()));

		// Frame header: 5 16-bit values:
		// 1. Offset to start of the pixel data.
		// 2. Width
		// 3. Height
		// 4..5. Unused (0)
		const size_t frameHeaderPos = clxData.size();
		clxData.resize(clxData.size() + ClxFrameHeaderSize);

		// Frame header:
		const uint16_t frameWidth = cropWidths.empty()
		    ? static_cast<uint16_t>(width)
		    : cropWidths[std::min<size_t>(cropWidths.size(), frame) - 1];

		WriteLE16(&clxData[frameHeaderPos], ClxFrameHeaderSize);
		WriteLE16(&clxData[frameHeaderPos + 2], static_cast<uint16_t>(frameWidth));
		WriteLE16(&clxData[frameHeaderPos + 4], static_cast<uint16_t>(frameHeight));

		for (unsigned j = 0; j < frameHeight; ++j) {
			for (unsigned x = 0; x < bytesPerLine;) {
				if (dataPtr == dataEnd)
					return IoError { "PCX pixel data is truncated" };
				constexpr uint8_t PcxMaxSinglePixel = 0xBF;
				const uint8_t byte = *dataPtr++;
				if (byte <= PcxMaxSinglePixel) {
					lineBuffer[x++] = byte;
					continue;
				}
				if (dataPtr == dataEnd)
					return IoError { "PCX pixel data is truncated" };
				constexpr uint8_t PcxRunLengthMask = 0x3F;
				const uint8_t runLength = (byte & PcxRunLengthMask);
				if (runLength > bytesPerLine - x)
					return IoError { "PCX run crosses a scanline boundary" };
				std::memset(&lineBuffer[x], *dataPtr++, runLength);
				x += runLength;
			}
			std::memcpy(&frameBuffer[static_cast<size_t>(j) * width], lineBuffer.get(), width);
		}

		unsigned transparentRunWidth = 0;
		size_t line = 0;
		while (line != frameHeight) {
			// Process line:
			const uint8_t *src = &frameBuffer[(frameHeight - (line + 1)) * width];
			if (transparentColor) {
				unsigned solidRunWidth = 0;
				for (const uint8_t *srcEnd = src + frameWidth; src != srcEnd; ++src) {
					if (*src == *transparentColor) {
						if (solidRunWidth != 0) {
							AppendClxPixelsOrFillRun(
							    src - transparentRunWidth - solidRunWidth, solidRunWidth,
							    clxData);
							solidRunWidth = 0;
						}
						++transparentRunWidth;
					} else {
						AppendClxTransparentRun(transparentRunWidth, clxData);
						transparentRunWidth = 0;
						++solidRunWidth;
					}
				}
				if (solidRunWidth != 0) {
					AppendClxPixelsOrFillRun(src - solidRunWidth, solidRunWidth, clxData);
				}
			} else {
				AppendClxPixelsOrFillRun(src, frameWidth, clxData);
			}
			++line;
		}
		AppendClxTransparentRun(transparentRunWidth, clxData);
	}
	WriteLE32(&clxData[4 * (1 + static_cast<size_t>(numFrames))],
	    static_cast<uint32_t>(clxData.size()));

	if (paletteData != nullptr) {
		// The 256-color palette is stored at the end of the file:
		// a separator byte followed by 256 RGB triplets.
		constexpr unsigned PcxPaletteSeparator = 0x0C;
		constexpr size_t PcxPaletteSize = 1 + 256 * 3;
		if (static_cast<size_t>(dataEnd - dataPtr) < PcxPaletteSize)
			return IoError { std::string("PCX has no palette") };
		dataPtr = dataEnd - PcxPaletteSize;
		if (*dataPtr++ != PcxPaletteSeparator)
			return IoError { std::string("PCX has no palette") };

		uint8_t *out = paletteData;
		for (unsigned i = 0; i < 256; ++i) {
			*out++ = *dataPtr++;
			*out++ = *dataPtr++;
			*out++ = *dataPtr++;
		}
	}
	return std::nullopt;
}

std::optional<IoError> PcxToClx(const char *inputPath, const char *outputPath,
    int numFramesOrFrameHeight,
    std::optional<uint8_t> transparentColor,
    const std::vector<uint16_t> &cropWidths,
    bool exportPalette,
    uintmax_t *inputFileSize,
    uintmax_t *outputFileSize)
{
	std::ifstream input;
	input.open(inputPath, std::ios::in | std::ios::binary);
	if (input.fail())
		return IoError { std::string("Failed to open input file: ")
			                 .append(std::strerror(errno)) };

	std::error_code ec;
	uintmax_t pixelDataSize = std::filesystem::file_size(inputPath, ec);
	if (ec)
		return IoError { ec.message() };
	if (inputFileSize != nullptr)
		*inputFileSize = pixelDataSize;

	std::unique_ptr<uint8_t[]> fileBuffer { new uint8_t[pixelDataSize] };
	input.read(reinterpret_cast<char *>(fileBuffer.get()), static_cast<std::streamsize>(pixelDataSize));
	if (input.fail()) {
		return IoError {
			std::string("Failed to read PCX data: ").append(std::strerror(errno))
		};
	}
	input.close();

	std::vector<uint8_t> clxData;
	std::array<uint8_t, 256 * 3> paletteData;
	if (const std::optional<IoError> error = PcxToClx(
	        fileBuffer.get(), pixelDataSize, numFramesOrFrameHeight, transparentColor,
	        cropWidths, clxData, exportPalette ? paletteData.data() : nullptr);
	    error.has_value()) {
		return error;
	}

	if (outputFileSize != nullptr)
		*outputFileSize = clxData.size();

	if (exportPalette) {
		std::ofstream output;
		output.open(std::filesystem::path(outputPath).replace_extension("pal").c_str(), std::ios::out | std::ios::binary);
		if (output.fail())
			return IoError { std::string("Failed to open palette output file: ")
				                 .append(std::strerror(errno)) };
		output.write(reinterpret_cast<const char *>(paletteData.data()), static_cast<std::streamsize>(paletteData.size()));
		output.close();
		if (output.fail())
			return IoError { std::string("Failed to write to palette output file: ")
				                 .append(std::strerror(errno)) };
	}

	{
		std::ofstream output;
		output.open(outputPath, std::ios::out | std::ios::binary);
		if (output.fail())
			return IoError { std::string("Failed to open output file: ")
				                 .append(std::strerror(errno)) };
		output.write(reinterpret_cast<const char *>(clxData.data()), static_cast<std::streamsize>(clxData.size()));
		output.close();
		if (output.fail())
			return IoError { std::string("Failed to write to output file: ")
				                 .append(std::strerror(errno)) };
	}

	return std::nullopt;
}

} // namespace dvl_gfx
