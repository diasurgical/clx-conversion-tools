#include <cl22clx.hpp>

#include <cassert>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include <clx_decode.hpp>
#include <clx_encode.hpp>
#include <dvl_gfx_endian.hpp>

namespace dvl_gfx {

namespace {

constexpr bool IsCl2Opaque(uint8_t control)
{
	constexpr uint8_t Cl2OpaqueMin = 0x80;
	return control >= Cl2OpaqueMin;
}

constexpr uint8_t GetCl2OpaquePixelsWidth(uint8_t control)
{
	return -static_cast<std::int8_t>(control);
}

constexpr bool IsCl2OpaqueFill(uint8_t control)
{
	constexpr uint8_t Cl2FillMax = 0xBE;
	return control <= Cl2FillMax;
}

constexpr uint8_t GetCl2OpaqueFillWidth(uint8_t control)
{
	constexpr uint8_t Cl2FillEnd = 0xBF;
	return static_cast<int_fast16_t>(Cl2FillEnd - control);
}

size_t CountCl2FramePixels(const uint8_t *src, const uint8_t *srcEnd)
{
	size_t numPixels = 0;
	while (src != srcEnd) {
		uint8_t val = *src++;
		if (IsCl2Opaque(val)) {
			if (IsCl2OpaqueFill(val)) {
				numPixels += GetCl2OpaqueFillWidth(val);
				++src;
			} else {
				val = GetCl2OpaquePixelsWidth(val);
				numPixels += val;
				src += val;
			}
		} else {
			numPixels += val;
		}
	}
	return numPixels;
}

struct SkipSize {
	int_fast16_t wholeLines;
	int_fast16_t xOffset;
};
SkipSize GetSkipSize(int_fast16_t overrun, int_fast16_t srcWidth)
{
	SkipSize result;
	result.wholeLines = overrun / srcWidth;
	result.xOffset = overrun - srcWidth * result.wholeLines;
	return result;
}

} // namespace

std::optional<IoError> Cl2ToClx(const uint8_t *data, size_t size,
    const uint16_t *widths, size_t numWidths,
    std::vector<uint8_t> &clxData)
{
	uint32_t numGroups = 1;
	const uint32_t maybeNumFrames = LoadLE32(data);
	const uint8_t *groupBegin = data;

	// If it is a number of frames, then the last frame offset will be equal to the size of the file.
	if (LoadLE32(&data[maybeNumFrames * 4 + 4]) != size) {
		// maybeNumFrames is the address of the first group, right after
		// the list of group offsets.
		numGroups = maybeNumFrames / 4;
		clxData.resize(maybeNumFrames);
	}

	// Transient buffer for a contiguous run of non-transparent pixels.
	std::vector<uint8_t> pixels;
	pixels.reserve(4096);

	for (size_t group = 0; group < numGroups; ++group) {
		uint32_t numFrames;
		if (numGroups == 1) {
			numFrames = maybeNumFrames;
		} else {
			groupBegin = &data[LoadLE32(&data[group * 4])];
			numFrames = LoadLE32(groupBegin);
			WriteLE32(&clxData[4 * group], clxData.size());
		}

		// CLX header: frame count, frame offset for each frame, file size
		const size_t clxDataOffset = clxData.size();
		clxData.resize(clxData.size() + 4 * (2 + static_cast<size_t>(numFrames)));
		WriteLE32(&clxData[clxDataOffset], numFrames);

		const uint8_t *frameEnd = &groupBegin[LoadLE32(&groupBegin[4])];
		for (size_t frame = 1; frame <= numFrames; ++frame) {
			WriteLE32(&clxData[clxDataOffset + 4 * frame],
			    static_cast<uint32_t>(clxData.size() - clxDataOffset));

			const uint8_t *frameBegin = frameEnd;
			frameEnd = &groupBegin[LoadLE32(&groupBegin[4 * (frame + 1)])];

			const uint16_t frameWidth = numWidths == 1 ? *widths : widths[frame - 1];

			const size_t frameHeaderPos = clxData.size();
			clxData.resize(clxData.size() + ClxFrameHeaderSize);
			WriteLE16(&clxData[frameHeaderPos], ClxFrameHeaderSize);
			WriteLE16(&clxData[frameHeaderPos + 2], frameWidth);

			unsigned transparentRunWidth = 0;
			int_fast16_t xOffset = 0;
			size_t frameHeight = 0;
			const uint8_t *src = frameBegin + LoadLE16(frameBegin);
			while (src != frameEnd) {
				auto remainingWidth = static_cast<int_fast16_t>(frameWidth) - xOffset;
				while (remainingWidth > 0) {
					const ClxBlitCommand cmd = ClxGetBlitCommand(src);
					switch (cmd.type) {
					case ClxBlitType::Transparent:
						if (!pixels.empty()) {
							AppendClxPixelsOrFillRun(pixels.data(), pixels.size(), clxData);
							pixels.clear();
						}

						transparentRunWidth += cmd.length;
						break;
					case ClxBlitType::Fill:
					case ClxBlitType::Pixels:
						AppendClxTransparentRun(transparentRunWidth, clxData);
						transparentRunWidth = 0;

						if (cmd.type == ClxBlitType::Fill) {
							pixels.insert(pixels.end(), cmd.length, cmd.color);
						} else { // ClxBlitType::Pixels
							pixels.insert(pixels.end(), src + 1, cmd.srcEnd);
						}
						break;
					}
					src = cmd.srcEnd;
					remainingWidth -= cmd.length;
				}

				// In CLX, only transparent runs can continue on the next line, so end the opaque run here.
				// Otherwise, a line that ends with opaque pixels would be joined with the opaque pixels
				// that begin the next line, and the renderer would draw those past the end of this line.
				if (!pixels.empty()) {
					AppendClxPixelsOrFillRun(pixels.data(), pixels.size(), clxData);
					pixels.clear();
				}

				++frameHeight;
				if (remainingWidth < 0) {
					const auto skipSize = GetSkipSize(-remainingWidth, static_cast<int_fast16_t>(frameWidth));
					xOffset = skipSize.xOffset;
					frameHeight += skipSize.wholeLines;
				} else {
					xOffset = 0;
				}
			}
			if (!pixels.empty()) {
				AppendClxPixelsOrFillRun(pixels.data(), pixels.size(), clxData);
				pixels.clear();
			}
			AppendClxTransparentRun(transparentRunWidth, clxData);

			WriteLE16(&clxData[frameHeaderPos + 4], frameHeight);
		}

		WriteLE32(&clxData[clxDataOffset + 4 * (1 + static_cast<size_t>(numFrames))], static_cast<uint32_t>(clxData.size() - clxDataOffset));
	}
	return std::nullopt;
}

std::optional<IoError> Cl2ToClxNoReencode(uint8_t *data, size_t size,
    const uint16_t *widths, size_t numWidths)
{
	uint32_t numGroups = 1;
	const uint32_t maybeNumFrames = LoadLE32(data);
	uint8_t *groupBegin = data;

	// If it is a number of frames, then the last frame offset will be equal to the size of the file.
	if (LoadLE32(&data[maybeNumFrames * 4 + 4]) != size) {
		// maybeNumFrames is the address of the first group, right after
		// the list of group offsets.
		numGroups = maybeNumFrames / 4;
	}

	for (size_t group = 0; group < numGroups; ++group) {
		uint32_t numFrames;
		if (numGroups == 1) {
			numFrames = maybeNumFrames;
		} else {
			groupBegin = &data[LoadLE32(&data[group * 4])];
			numFrames = LoadLE32(groupBegin);
		}

		uint8_t *frameEnd = &groupBegin[LoadLE32(&groupBegin[4])];
		for (size_t frame = 1; frame <= numFrames; ++frame) {
			uint8_t *frameBegin = frameEnd;
			frameEnd = &groupBegin[LoadLE32(&groupBegin[4 * (frame + 1)])];

			const size_t numPixels = CountCl2FramePixels(frameBegin + LoadLE16(frameBegin), frameEnd);

			const uint16_t frameWidth = numWidths == 1 ? *widths : widths[frame - 1];
			const uint16_t frameHeight = numPixels / frameWidth;
			WriteLE16(&frameBegin[2], frameWidth);
			WriteLE16(&frameBegin[4], frameHeight);
			memset(&frameBegin[6], 0, 4);
		}
	}
	return std::nullopt;
}

std::optional<IoError> Cl2ToClx(const char *inputPath, const char *outputPath,
    const uint16_t *widths, size_t numWidths, bool reencode)
{
	std::error_code ec;
	const uintmax_t size = std::filesystem::file_size(inputPath, ec);
	if (ec)
		return IoError { ec.message() };

	std::ifstream input;
	input.open(inputPath, std::ios::in | std::ios::binary);
	if (input.fail())
		return IoError { std::string("Failed to open input file: ")
			                 .append(std::strerror(errno)) };
	std::unique_ptr<uint8_t[]> ownedData { new uint8_t[size] };
	input.read(reinterpret_cast<char *>(ownedData.get()), static_cast<std::streamsize>(size));
	if (input.fail()) {
		return IoError {
			std::string("Failed to read CL2 data: ").append(std::strerror(errno))
		};
	}
	input.close();

	std::ofstream output;
	output.open(outputPath, std::ios::out | std::ios::binary);
	if (output.fail())
		return IoError { std::string("Failed to open output file: ")
			                 .append(std::strerror(errno)) };

	if (reencode) {
		std::vector<uint8_t> out;
		std::optional<IoError> result = Cl2ToClx(ownedData.get(), size, widths, numWidths, out);
		if (result.has_value())
			return result;
		output.write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size()));
	} else {
		std::optional<IoError> result = Cl2ToClxNoReencode(ownedData.get(), size, widths, numWidths);
		if (result.has_value())
			return result;
		output.write(reinterpret_cast<const char *>(ownedData.get()), static_cast<std::streamsize>(size));
	}

	output.close();
	if (output.fail())
		return IoError { std::string("Failed to write to output file: ")
			                 .append(std::strerror(errno)) };
	return std::nullopt;
}

std::optional<IoError> CombineCl2AsClxSheet(
    const char *const *inputPaths, size_t numFiles, const char *outputPath,
    const std::vector<uint16_t> &widths, bool reencode)
{
	size_t accumulatedSize = ClxSheetHeaderSize(numFiles);
	std::vector<size_t> offsets;
	offsets.reserve(numFiles + 1);
	for (size_t i = 0; i < numFiles; ++i) {
		const char *inputPath = inputPaths[i];
		std::ifstream input;
		std::error_code ec;
		const uintmax_t size = std::filesystem::file_size(inputPath, ec);
		if (ec)
			return IoError { ec.message() };
		offsets.push_back(accumulatedSize);
		accumulatedSize += size;
	}
	offsets.push_back(accumulatedSize);
	std::unique_ptr<uint8_t[]> ownedData { new uint8_t[accumulatedSize] };
	for (size_t i = 0; i < numFiles; ++i) {
		ClxSheetHeaderSetListOffset(i, offsets[i], ownedData.get());
		std::ifstream input;
		input.open(inputPaths[i], std::ios::in | std::ios::binary);
		if (input.fail()) {
			return IoError { std::string("Failed to open input file: ")
				                 .append(std::strerror(errno)) };
		}
		input.read(reinterpret_cast<char *>(&ownedData[offsets[i]]), static_cast<std::streamsize>(offsets[i + 1] - offsets[i]));
		if (input.fail()) {
			return IoError {
				std::string("Failed to read CL2 data: ").append(std::strerror(errno))
			};
		}
		input.close();
	}

	std::ofstream output;
	if (reencode) {
		std::vector<uint8_t> out;
		if (std::optional<IoError> error = Cl2ToClx(
		        ownedData.get(), accumulatedSize, widths.data(), widths.size(), out);
		    error.has_value()) {
			return error;
		}
		output.open(outputPath, std::ios::out | std::ios::binary);
		if (output.fail()) {
			return IoError { std::string("Failed to open output file: ")
				                 .append(std::strerror(errno)) };
		}
		output.write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size()));
	} else {
		if (std::optional<IoError> error = Cl2ToClxNoReencode(
		        ownedData.get(), accumulatedSize, widths.data(), widths.size());
		    error.has_value()) {
			return error;
		}
		output.open(outputPath, std::ios::out | std::ios::binary);
		if (output.fail()) {
			return IoError { std::string("Failed to open output file: ")
				                 .append(std::strerror(errno)) };
		}
		output.write(reinterpret_cast<const char *>(ownedData.get()), static_cast<std::streamsize>(accumulatedSize));
	}
	output.close();
	if (output.fail())
		return IoError { std::string("Failed to write to output file: ")
			                 .append(std::strerror(errno)) };
	return std::nullopt;
}

} // namespace dvl_gfx
