#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <clx_encode.hpp>
#include <dvl_gfx_common.hpp>
#include <pixels2clx.hpp>

#include "argument_parser.hpp"
#include "png_codec.hpp"
#include "tl/expected.hpp"

namespace dvl_gfx {
namespace {

constexpr char KHelp[] = R"(Usage: png2clx [options] files...

Converts palettized PNG sprite(s) to a CLX file.

Each PNG contains one or more lists of sprites, laid out side by side
(see --num-lists). Within each list, the sprites are stacked vertically
(see --num-sprites). If there is more than one list, a CLX sheet is created.
This is the layout produced by clx2png.

Options:
  --output-dir <arg>           Output directory. Default: input file directory.
  --output-filename <arg>      Output filename. Default: input basename with the ".clx" extension.
                               With --combine, the default is the basename of the first file without
                               the trailing digits.
  --transparent-color <arg>    Transparent color index.
                               Default: the first fully transparent palette entry (PNG tRNS chunk), if any.
  --no-transparent-color       Do not use a transparent color, even if the PNG has one.
  --num-sprites <arg>          The number of vertically-stacked sprites in each list. Default: 1.
  --num-lists <arg>            The number of equal-width sprite lists laid out side by side in each PNG.
                               Default: 1.
  --combine                    Combine the lists from multiple PNG files into a single CLX sheet.
  --export-palette             Export the palette as a .pal file.
  --remove                     Remove the input files.
  -q, --quiet                  Do not log anything.
)";

struct Options {
	std::vector<const char *> inputPaths;
	std::optional<std::string_view> outputDir;
	std::optional<std::string_view> outputFilename;
	uint16_t numSprites = 1;
	uint16_t numLists = 1;
	std::optional<uint8_t> transparentColor;
	bool noTransparentColor = false;
	bool combine = false;
	bool exportPalette = false;
	bool remove = false;
	bool quiet = false;
};

void PrintHelp()
{
	std::cerr << KHelp << std::endl;
}

tl::expected<Options, ArgumentError> ParseArguments(int argc, char *argv[])
{
	if (argc == 1) {
		PrintHelp();
		std::exit(64);
	}
	Options options;
	ArgumentParserState state { 1, argc, argv };
	for (; !state.atEnd(); ++state.pos) {
		const std::string_view arg = state.arg();
		if (arg == "-h" || arg == "--help") {
			PrintHelp();
			std::exit(0);
		}
		if (arg == "--output-dir") {
			tl::expected<std::string_view, ArgumentError> value = ParseArgumentValue(state);
			if (!value.has_value())
				return tl::unexpected { std::move(value).error() };
			options.outputDir = *value;
		} else if (arg == "--output-filename") {
			tl::expected<std::string_view, ArgumentError> value = ParseArgumentValue(state);
			if (!value.has_value())
				return tl::unexpected { std::move(value).error() };
			options.outputFilename = *value;
		} else if (arg == "--num-sprites") {
			tl::expected<uint16_t, ArgumentError> value = ParseIntArgument<uint16_t>(state, 1);
			if (!value.has_value())
				return tl::unexpected { std::move(value).error() };
			options.numSprites = *value;
		} else if (arg == "--num-lists") {
			tl::expected<uint16_t, ArgumentError> value = ParseIntArgument<uint16_t>(state, 1);
			if (!value.has_value())
				return tl::unexpected { std::move(value).error() };
			options.numLists = *value;
		} else if (arg == "--transparent-color") {
			tl::expected<uint8_t, ArgumentError> value = ParseIntArgument<uint8_t>(state);
			if (!value.has_value())
				return tl::unexpected { std::move(value).error() };
			options.transparentColor = *value;
		} else if (arg == "--no-transparent-color") {
			options.noTransparentColor = true;
		} else if (arg == "--combine") {
			options.combine = true;
		} else if (arg == "--export-palette") {
			options.exportPalette = true;
		} else if (arg == "--remove") {
			options.remove = true;
		} else if (arg == "-q" || arg == "--quiet") {
			options.quiet = true;
		} else if (arg.empty() || arg[0] == '-') {
			return tl::unexpected { ArgumentError { arg, "unknown argument" } };
		} else {
			break;
		}
	}
	if (options.noTransparentColor && options.transparentColor.has_value()) {
		return tl::unexpected { ArgumentError { "--no-transparent-color", "cannot be used with --transparent-color" } };
	}
	if (std::optional<ArgumentError> error = ParsePositionalArguments(state, "files...", options.inputPaths);
	    error.has_value()) {
		return tl::unexpected { *std::move(error) };
	}
	if (!options.combine && options.outputFilename.has_value() && options.inputPaths.size() > 1) {
		return tl::unexpected { ArgumentError {
			"--output-filename", "Cannot pass more than one input path with --output-filename and without --combine" } };
	}
	if (options.combine && options.inputPaths.size() < 2) {
		return tl::unexpected { ArgumentError {
			"--combine", "requires at least 2 input files" } };
	}
	return options;
}

std::string DefaultCombinedFilename(const Options &options)
{
	std::string outputFilename = std::filesystem::path(options.inputPaths[0]).stem().string();
	size_t numSuffixLength = 0;
	while (numSuffixLength < outputFilename.size()) {
		const char c = outputFilename[outputFilename.size() - numSuffixLength - 1];
		if (c < '0' || c > '9')
			break;
		++numSuffixLength;
	}
	outputFilename.resize(outputFilename.size() - numSuffixLength);
	outputFilename.append(".clx");
	return outputFilename;
}

std::optional<IoError> LoadPng(const char *inputPath, PalettedImage &image, uintmax_t &fileSize)
{
	std::error_code ec;
	fileSize = std::filesystem::file_size(inputPath, ec);
	if (ec)
		return IoError { ec.message() };

	std::ifstream input;
	input.open(inputPath, std::ios::in | std::ios::binary);
	if (input.fail())
		return IoError { std::string("Failed to open input file: ")
			                 .append(std::strerror(errno)) };
	std::vector<uint8_t> fileData(fileSize);
	input.read(reinterpret_cast<char *>(fileData.data()), static_cast<std::streamsize>(fileSize));
	if (input.fail()) {
		return IoError {
			std::string("Failed to read PNG data: ").append(std::strerror(errno))
		};
	}
	input.close();
	return PngDecodePaletted(fileData, image);
}

/**
 * @brief Converts the sprite lists in the image to CLX lists and appends them to `lists`.
 */
std::optional<IoError> AppendClxLists(const PalettedImage &image, const Options &options,
    std::vector<std::vector<uint8_t>> &lists)
{
	if (image.size.width % options.numLists != 0) {
		return IoError { std::string("Image width (")
			                 .append(std::to_string(image.size.width))
			                 .append(") is not divisible by the number of lists (")
			                 .append(std::to_string(options.numLists))
			                 .append(")") };
	}
	const unsigned listWidth = image.size.width / options.numLists;
	if (listWidth > std::numeric_limits<uint16_t>::max())
		return IoError { "Sprite is too wide" };
	if (image.size.height % options.numSprites != 0) {
		return IoError { std::string("Image height (")
			                 .append(std::to_string(image.size.height))
			                 .append(") is not divisible by the number of sprites (")
			                 .append(std::to_string(options.numSprites))
			                 .append(")") };
	}
	const unsigned frameHeight = image.size.height / options.numSprites;
	if (frameHeight > std::numeric_limits<uint16_t>::max())
		return IoError { "Sprite is too tall" };

	std::optional<uint8_t> transparentColor;
	if (options.transparentColor.has_value()) {
		transparentColor = options.transparentColor;
	} else if (!options.noTransparentColor) {
		transparentColor = image.transparentColor;
	}

	for (unsigned i = 0; i < options.numLists; ++i) {
		Pixels2Clx(&image.pixels[static_cast<size_t>(i) * listWidth], image.size.width,
		    listWidth, frameHeight, options.numSprites, transparentColor, lists.emplace_back());
	}
	return std::nullopt;
}

/**
 * @brief Returns a CLX list if there is only one list, or a CLX sheet otherwise.
 */
std::vector<uint8_t> BuildClx(std::vector<std::vector<uint8_t>> &lists)
{
	if (lists.size() == 1)
		return std::move(lists[0]);

	const auto numLists = static_cast<uint32_t>(lists.size());
	size_t size = ClxSheetHeaderSize(numLists);
	for (const std::vector<uint8_t> &list : lists)
		size += list.size();

	std::vector<uint8_t> clxData;
	clxData.reserve(size);
	clxData.resize(ClxSheetHeaderSize(numLists));
	for (uint32_t i = 0; i < numLists; ++i) {
		ClxSheetHeaderSetListOffset(i, static_cast<uint32_t>(clxData.size()), clxData.data());
		clxData.insert(clxData.end(), lists[i].begin(), lists[i].end());
	}
	return clxData;
}

std::optional<IoError> WriteFile(const std::filesystem::path &path, std::span<const uint8_t> data)
{
	std::ofstream output;
	output.open(path, std::ios::out | std::ios::binary);
	if (output.fail())
		return IoError { std::string("Failed to open output file: ")
			                 .append(std::strerror(errno)) };
	output.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
	output.close();
	if (output.fail())
		return IoError { std::string("Failed to write to output file: ")
			                 .append(std::strerror(errno)) };
	return std::nullopt;
}

/**
 * @brief Converts the given PNG files to a single CLX file.
 */
std::optional<IoError> PngToClx(std::span<const char *const> inputPaths,
    const std::filesystem::path &outputPath, const Options &options,
    uintmax_t &inputFileSize, uintmax_t &outputFileSize)
{
	inputFileSize = 0;
	std::vector<std::vector<uint8_t>> lists;
	std::optional<std::array<uint8_t, 256 * 3>> palette;
	for (const char *inputPath : inputPaths) {
		PalettedImage image;
		uintmax_t fileSize;
		std::optional<IoError> error = LoadPng(inputPath, image, fileSize);
		if (!error.has_value())
			error = AppendClxLists(image, options, lists);
		if (error.has_value()) {
			error->message.append(": ").append(inputPath);
			return error;
		}
		inputFileSize += fileSize;

		if (!palette.has_value()) {
			palette = image.palette;
		} else if (options.exportPalette && *palette != image.palette) {
			return IoError { std::string("Cannot export the palette because the input files have different palettes: ")
				                 .append(inputPaths[0])
				                 .append(", ")
				                 .append(inputPath) };
		}
	}

	if (lists.size() > std::numeric_limits<uint16_t>::max())
		return IoError { "Too many lists" };
	const std::vector<uint8_t> clxData = BuildClx(lists);

	if (options.exportPalette) {
		if (std::optional<IoError> error = WriteFile(
		        std::filesystem::path(outputPath).replace_extension("pal"), *palette);
		    error.has_value()) {
			return error;
		}
	}
	if (std::optional<IoError> error = WriteFile(outputPath, clxData); error.has_value()) {
		return error;
	}
	outputFileSize = clxData.size();
	return std::nullopt;
}

std::optional<IoError> Run(const Options &options)
{
	if (!options.quiet) {
		std::clog << "file\tPNG\tCLX" << std::endl;
	}

	std::optional<std::filesystem::path> outputDirFs;
	if (options.outputDir.has_value())
		outputDirFs = *options.outputDir;

	const auto getOutputPath = [&](const std::filesystem::path &inputPathFs, const std::filesystem::path &filename) {
		if (outputDirFs.has_value())
			return *outputDirFs / filename;
		return std::filesystem::path(inputPathFs).replace_filename(filename);
	};

	const auto convert = [&](std::span<const char *const> inputPaths, const std::filesystem::path &outputPath) -> std::optional<IoError> {
		uintmax_t inputFileSize;
		uintmax_t outputFileSize;
		if (std::optional<IoError> error = PngToClx(inputPaths, outputPath, options, inputFileSize, outputFileSize);
		    error.has_value()) {
			return error;
		}
		if (options.remove) {
			for (const char *inputPath : inputPaths)
				std::filesystem::remove(inputPath);
		}
		if (!options.quiet) {
			std::clog << outputPath.stem().string() << "\t" << inputFileSize << "\t"
			          << outputFileSize << std::endl;
		}
		return std::nullopt;
	};

	if (options.combine) {
		const std::filesystem::path outputFilename = options.outputFilename.has_value()
		    ? std::filesystem::path(*options.outputFilename)
		    : std::filesystem::path(DefaultCombinedFilename(options));
		return convert(options.inputPaths, getOutputPath(options.inputPaths[0], outputFilename));
	}

	for (const char *inputPath : options.inputPaths) {
		const std::filesystem::path inputPathFs { inputPath };
		const std::filesystem::path outputFilename = options.outputFilename.has_value()
		    ? std::filesystem::path(*options.outputFilename)
		    : inputPathFs.filename().replace_extension("clx");
		if (std::optional<IoError> error = convert(std::span(&inputPath, 1), getOutputPath(inputPathFs, outputFilename));
		    error.has_value()) {
			return error;
		}
	}
	return std::nullopt;
}

} // namespace
} // namespace dvl_gfx

int main(int argc, char *argv[])
{
	tl::expected<dvl_gfx::Options, dvl_gfx::ArgumentError> options = dvl_gfx::ParseArguments(argc, argv);
	if (!options) {
		std::cerr << options.error().arg << ": " << options.error().error
		          << std::endl;
		return 64;
	}
	if (std::optional<dvl_gfx::IoError> error = Run(*options);
	    error.has_value()) {
		std::cerr << error->message << std::endl;
		return 1;
	}
	return 0;
}
