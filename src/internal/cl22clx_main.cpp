#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cl22clx.hpp>
#include <dvl_gfx_common.hpp>

#include "argument_parser.hpp"
#include "tl/expected.hpp"

namespace dvl_gfx {
namespace {

constexpr char KHelp[] = R"(Usage: cl22clx [options] files...

Converts CL2 sprite(s) to a CLX file.

Options:
  --output-dir <arg>           Output directory. Default: input file directory.
  --output-filename <arg>      Output filename. Default: input basename with the ".clx" extension.
                               With --combine, the default is the basename of the first file without
                               the trailing digits.
  --width <arg>[,<arg>...]     CL2 sprite frame width(s), comma-separated.
  --combine                    Combine multiple CL2 files into a single CLX sheet.
  --no-reencode                Do not reencode graphics data with the more optimal DevilutionX encoder.
  --remove                     Remove the input files.
  -q, --quiet                  Do not log anything.
)";

struct Options {
	std::vector<const char *> inputPaths;
	std::optional<std::string_view> outputDir;
	std::optional<std::string_view> outputFilename;
	std::vector<uint16_t> widths;
	bool combine = false;
	bool remove = false;
	bool reencode = true;
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
		} else if (arg == "--width") {
			tl::expected<std::vector<uint16_t>, ArgumentError> value = ParseIntListArgument<uint16_t>(state, 1);
			if (!value.has_value())
				return tl::unexpected { std::move(value).error() };
			options.widths = *std::move(value);
		} else if (arg == "--combine") {
			options.combine = true;
		} else if (arg == "--no-reencode") {
			options.reencode = false;
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
	if (!options.combine && options.outputFilename.has_value() && options.inputPaths.size() > 1) {
		return tl::unexpected { ArgumentError {
			"--output-filename", "Cannot pass more than one input path with --output-filename and without --combine" } };
	}
	if (options.combine && options.inputPaths.size() < 2) {
		return tl::unexpected { ArgumentError {
			"--combine", "requires at least 2 input files" } };
	}
	if (options.widths.empty()) {
		return tl::unexpected { ArgumentError { "--width", "is required" } };
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

std::optional<IoError> Run(const Options &options)
{
	std::optional<std::filesystem::path> outputDirFs;
	if (options.outputDir.has_value())
		outputDirFs = *options.outputDir;

	if (options.combine) {
		std::string outputFilename;
		if (options.outputFilename.has_value()) {
			outputFilename = *options.outputFilename;
		} else {
			outputFilename = DefaultCombinedFilename(options);
		}
		std::filesystem::path outputPath;
		if (outputDirFs.has_value()) {
			outputPath = *outputDirFs / outputFilename;
		} else {
			outputPath = std::filesystem::path(options.inputPaths[0]).parent_path() / outputFilename;
		}
		std::optional<dvl_gfx::IoError> error = CombineCl2AsClxSheet(
		    options.inputPaths.data(), options.inputPaths.size(),
		    outputPath.string().c_str(), options.widths, options.reencode);
		if (error.has_value())
			return error;
		return std::nullopt;
	} else {
		for (const char *inputPath : options.inputPaths) {
			std::filesystem::path inputPathFs { inputPath };
			std::string outputFilename;
			if (options.outputFilename.has_value()) {
				outputFilename = *options.outputFilename;
			} else {
				outputFilename = inputPathFs.filename().replace_extension("clx").string();
			}
			std::filesystem::path outputPath;
			if (outputDirFs.has_value()) {
				outputPath = *outputDirFs / outputFilename;
			} else {
				outputPath = inputPathFs.parent_path() / outputFilename;
			}
			if (std::optional<dvl_gfx::IoError> error = Cl2ToClx(
			        inputPath, outputPath.string().c_str(),
			        options.widths, options.reencode);
			    error.has_value()) {
				error->message.append(": ").append(inputPath);
				return error;
			}
			if (options.remove) {
				std::filesystem::remove(inputPathFs);
			}
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
