#include <cl22clx.hpp>

#include <cassert>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
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

IoError FrameWidthMismatchError(uint16_t width)
{
	return IoError { std::string("CL2 frame data does not match the frame width (")
		                 .append(std::to_string(width))
		                 .append(")") };
}

/**
 * @brief Counts the pixels in the CL2 frame data and validates it against the frame width.
 */
std::optional<IoError> CountCl2FramePixels(const uint8_t *src, const uint8_t *srcEnd, uint16_t width, size_t &numPixels)
{
	numPixels = 0;
	while (src != srcEnd) {
		uint8_t val = *src++;
		if (IsCl2Opaque(val)) {
			if (IsCl2OpaqueFill(val)) {
				if (src == srcEnd)
					return IoError { "CL2 frame data is truncated" };
				val = GetCl2OpaqueFillWidth(val);
				++src;
			} else {
				val = GetCl2OpaquePixelsWidth(val);
				if (val > srcEnd - src)
					return IoError { "CL2 frame data is truncated" };
				src += val;
			}
			// Only transparent runs can continue on the next line.
			if (numPixels % width + val > width)
				return FrameWidthMismatchError(width);
		}
		numPixels += val;
	}
	if (numPixels % width != 0)
		return FrameWidthMismatchError(width);
	return std::nullopt;
}

/**
 * @brief Returns the size of the CL2 / CLX command at `src`.
 */
size_t GetCommandSize(const uint8_t *src)
{
	const uint8_t control = *src;
	if (!IsClxOpaque(control))
		return 1;
	if (IsClxOpaqueFill(control))
		return 2;
	return 1 + static_cast<size_t>(GetClxOpaquePixelsWidth(control));
}

/**
 * @brief A list of CL2 frames: frame count, frame offsets, frame data.
 */
struct Cl2Group {
	size_t offset;
	/** @brief The maximum size of the group (frame offsets must be within this). */
	size_t size;
	uint32_t numFrames;
};

std::optional<IoError> ValidateCl2Group(const uint8_t *data, Cl2Group &group)
{
	const uint8_t *groupBegin = &data[group.offset];
	if (group.size < 8)
		return IoError { "CL2 data is truncated" };
	group.numFrames = LoadLE32(groupBegin);
	const uint64_t headerSize = 4 * (static_cast<uint64_t>(group.numFrames) + 2);
	if (headerSize > group.size)
		return IoError { "CL2 frame count is out of bounds" };
	uint32_t prevOffset = LoadLE32(&groupBegin[4]);
	if (prevOffset < headerSize)
		return IoError { "CL2 frame offset is out of bounds" };
	for (uint32_t frame = 1; frame <= group.numFrames; ++frame) {
		const uint32_t offset = LoadLE32(&groupBegin[4 * (frame + 1)]);
		if (offset < prevOffset || offset > group.size)
			return IoError { "CL2 frame offset is out of bounds" };
		if (offset - prevOffset < 2 || LoadLE16(&groupBegin[prevOffset]) > offset - prevOffset)
			return IoError { "CL2 frame header is out of bounds" };
		prevOffset = offset;
	}
	return std::nullopt;
}

struct Cl2Layout {
	std::vector<Cl2Group> groups;

	/**
	 * @brief Whether the CL2 starts with a list of group offsets (converted to a CLX sheet).
	 *
	 * If false, the CL2 is a single list of frames (converted to a CLX list).
	 */
	bool isGrouped;
};

/**
 * @brief Parses and validates the structure of a CL2 file.
 */
std::optional<IoError> GetCl2Layout(const uint8_t *data, size_t size, Cl2Layout &layout)
{
	if (size < 8)
		return IoError { "CL2 data is truncated" };
	const uint32_t maybeNumFrames = LoadLE32(data);
	const uint64_t maybeLastFrameOffsetPos = 4 * static_cast<uint64_t>(maybeNumFrames) + 4;

	// If it is a number of frames, then the last frame offset will be equal to the size of the file.
	if (maybeLastFrameOffsetPos + 4 <= size && LoadLE32(&data[maybeLastFrameOffsetPos]) == size) {
		layout.isGrouped = false;
		layout.groups.push_back(Cl2Group { 0, size, 0 });
	} else {
		// maybeNumFrames is the address of the first group, right after
		// the list of group offsets.
		layout.isGrouped = true;
		const uint32_t headerSize = maybeNumFrames;
		if (headerSize == 0 || headerSize % 4 != 0 || headerSize > size)
			return IoError { "Invalid CL2 header" };
		const uint32_t numGroups = headerSize / 4;
		for (uint32_t i = 0; i < numGroups; ++i) {
			// The frame offsets of all groups come first, followed by the frame data of all groups.
			// Frame offsets are relative to the start of the group, so a group spans until the end of the file.
			const uint32_t begin = LoadLE32(&data[4 * i]);
			if (begin < headerSize || begin > size)
				return IoError { "CL2 group offset is out of bounds" };
			layout.groups.push_back(Cl2Group { begin, size - begin, 0 });
		}
	}
	for (Cl2Group &group : layout.groups) {
		if (std::optional<IoError> error = ValidateCl2Group(data, group); error.has_value())
			return error;
	}
	return std::nullopt;
}

std::optional<IoError> GetFrameWidth(const uint16_t *widths, size_t numWidths, size_t frame, uint16_t &width)
{
	if (numWidths == 0)
		return IoError { "Frame width is required" };
	if (numWidths != 1 && frame > numWidths) {
		return IoError { std::string("Not enough frame widths: got ")
			                 .append(std::to_string(numWidths))
			                 .append(", frame count is at least ")
			                 .append(std::to_string(frame)) };
	}
	width = numWidths == 1 ? *widths : widths[frame - 1];
	if (width == 0)
		return IoError { "Frame width must not be 0" };
	return std::nullopt;
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
	Cl2Layout cl2;
	if (std::optional<IoError> error = GetCl2Layout(data, size, cl2); error.has_value())
		return error;
	if (cl2.isGrouped)
		clxData.resize(ClxSheetHeaderSize(cl2.groups.size()));

	// Transient buffer for a contiguous run of non-transparent pixels.
	std::vector<uint8_t> pixels;
	pixels.reserve(4096);

	for (size_t group = 0; group < cl2.groups.size(); ++group) {
		const uint8_t *groupBegin = &data[cl2.groups[group].offset];
		const uint32_t numFrames = cl2.groups[group].numFrames;
		if (cl2.isGrouped)
			ClxSheetHeaderSetListOffset(group, static_cast<uint32_t>(clxData.size()), clxData.data());

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

			uint16_t frameWidth;
			if (std::optional<IoError> error = GetFrameWidth(widths, numWidths, frame, frameWidth); error.has_value())
				return error;

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
					if (src == frameEnd)
						return FrameWidthMismatchError(frameWidth);
					if (GetCommandSize(src) > static_cast<size_t>(frameEnd - src))
						return IoError { "CL2 frame data is truncated" };
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
						// Only transparent runs can continue on the next line (both in CL2 and CLX).
						if (static_cast<int_fast16_t>(cmd.length) > remainingWidth)
							return FrameWidthMismatchError(frameWidth);

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
			if (xOffset != 0)
				return FrameWidthMismatchError(frameWidth);
			if (frameHeight > std::numeric_limits<uint16_t>::max())
				return IoError { "CL2 frame is too tall" };
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
	Cl2Layout cl2;
	if (std::optional<IoError> error = GetCl2Layout(data, size, cl2); error.has_value())
		return error;

	for (const Cl2Group &group : cl2.groups) {
		uint8_t *groupBegin = &data[group.offset];
		uint8_t *frameEnd = &groupBegin[LoadLE32(&groupBegin[4])];
		for (size_t frame = 1; frame <= group.numFrames; ++frame) {
			uint8_t *frameBegin = frameEnd;
			frameEnd = &groupBegin[LoadLE32(&groupBegin[4 * (frame + 1)])];

			// The CL2 frame header is 10 bytes, which we overwrite with the CLX frame header (padded).
			constexpr uint16_t Cl2FrameHeaderSize = 10;
			if (LoadLE16(frameBegin) < Cl2FrameHeaderSize)
				return IoError { "CL2 frame header is too small" };

			uint16_t frameWidth;
			if (std::optional<IoError> error = GetFrameWidth(widths, numWidths, frame, frameWidth); error.has_value())
				return error;
			size_t numPixels;
			if (std::optional<IoError> error = CountCl2FramePixels(frameBegin + LoadLE16(frameBegin), frameEnd, frameWidth, numPixels);
			    error.has_value()) {
				return error;
			}
			if (numPixels / frameWidth > std::numeric_limits<uint16_t>::max())
				return IoError { "CL2 frame is too tall" };
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

		Cl2Layout cl2;
		if (std::optional<IoError> error = GetCl2Layout(&ownedData[offsets[i]], offsets[i + 1] - offsets[i], cl2);
		    error.has_value()) {
			error->message.append(": ").append(inputPaths[i]);
			return error;
		}
		if (cl2.isGrouped) {
			return IoError { std::string("Only CL2 files with a single list of frames can be combined: ")
				                 .append(inputPaths[i]) };
		}
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
