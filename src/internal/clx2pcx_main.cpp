#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <clx2pixels.hpp>
#include <dvl_gfx_common.hpp>
#include <pcx_encode.hpp>

#include "argument_parser.hpp"
#include "palette.hpp"
#include "tl/expected.hpp"

namespace dvl_gfx {
namespace {

constexpr char KHelp[] = R"(Usage: clx2pcx [options] files...

Converts a CLX file to PCX.

Options:
  --output-dir <arg>           Output directory. Default: input file directory.
  --transparent-color <arg>    Transparent color index. Default: 255.
  --palette <arg>              default, diablo_menu, hellfire_menu, or a path to a .pal file.
  --remove                     Remove the input files.
  -q, --quiet                  Do not log anything.
)";

struct Options {
	std::vector<const char *> inputPaths;
	std::optional<std::string_view> outputDir;
	uint8_t transparentColor = 255;
	std::string_view palette = "default";
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
		} else if (arg == "--transparent-color") {
			tl::expected<uint8_t, ArgumentError> value = ParseIntArgument<uint8_t>(state);
			if (!value.has_value())
				return tl::unexpected { std::move(value).error() };
			options.transparentColor = *value;
		} else if (arg == "--palette") {
			tl::expected<std::string_view, ArgumentError> value = ParseArgumentValue(state);
			if (!value.has_value())
				return tl::unexpected { std::move(value).error() };
			options.palette = *value;
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
	if (std::optional<ArgumentError> error = ParsePositionalArguments(state, "files...", options.inputPaths);
	    error.has_value()) {
		return tl::unexpected { *std::move(error) };
	}
	return options;
}

std::optional<IoError> Run(const Options &options)
{
	if (!options.quiet) {
		std::clog << "file\tCLX\tPCX" << std::endl;
	}

	std::optional<std::filesystem::path> outputDirFs;
	if (options.outputDir.has_value())
		outputDirFs = *options.outputDir;

	std::array<uint8_t, PaletteSize> palette;
	if (std::optional<IoError> error = LoadPalette(options.palette, palette); error.has_value()) {
		return error;
	}

	std::vector<uint8_t> pixels;
	for (const char *inputPath : options.inputPaths) {
		std::filesystem::path inputPathFs { inputPath };
		std::filesystem::path outputPath;
		if (outputDirFs.has_value()) {
			outputPath = *outputDirFs / inputPathFs.filename().replace_extension("pcx");
		} else {
			outputPath = std::filesystem::path(inputPathFs).replace_extension("pcx");
		}

		Size dimensions;
		uintmax_t inputFileSize;
		{
			std::error_code ec;
			inputFileSize = std::filesystem::file_size(inputPath, ec);
			if (ec)
				return IoError { ec.message() };

			std::ifstream input;
			input.open(inputPath, std::ios::in | std::ios::binary);
			if (input.fail())
				return IoError { std::string("Failed to open input file: ")
					                 .append(std::strerror(errno)) };
			std::unique_ptr<uint8_t[]> ownedData { new uint8_t[inputFileSize] };
			input.read(reinterpret_cast<char *>(ownedData.get()), static_cast<std::streamsize>(inputFileSize));
			if (input.fail()) {
				return IoError {
					std::string("Failed to read CLX data: ").append(std::strerror(errno))
				};
			}
			input.close();
			std::span<const uint8_t> clxData(ownedData.get(), inputFileSize);
			if (std::optional<IoError> error = Clx2Pixels(
			        clxData, options.transparentColor, pixels,
			        /*pitch=*/std::nullopt, &dimensions);
			    error.has_value()) {
				error->message.append(": ").append(inputPath);
				return error;
			}
		}

		uintmax_t outputFileSize;
		std::ofstream output;
		output.open(outputPath, std::ios::out | std::ios::binary);
		if (output.fail())
			return IoError { std::string("Failed to open output file: ")
				                 .append(std::strerror(errno)) };

		std::optional<IoError> result = PcxEncode(
		    std::span(pixels.data(), dimensions.width * dimensions.height), dimensions,
		    dimensions.width, std::span(palette.data(), palette.size()), &output);
		if (result.has_value())
			return result;
		output.close();
		if (output.fail())
			return IoError { std::string("Failed to write to output file: ")
				                 .append(std::strerror(errno)) };

		if (options.remove) {
			std::filesystem::remove(inputPathFs);
		}
		if (!options.quiet) {
			std::error_code ec;
			const uintmax_t outputFileSize = std::filesystem::file_size(outputPath, ec);
			if (ec)
				return IoError { ec.message() };

			std::clog << inputPathFs.stem().string() << "\t" << inputFileSize << "\t"
			          << outputFileSize << std::endl;
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
