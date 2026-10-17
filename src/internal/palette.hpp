#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include <dvl_gfx_common.hpp>
#include <dvl_gfx_embedded_palettes.h>

namespace dvl_gfx {

constexpr size_t PaletteSize = 768;

inline std::optional<IoError> LoadPaletteFile(std::string_view path, std::array<uint8_t, PaletteSize> &palette)
{
	std::error_code ec;
	const uintmax_t size = std::filesystem::file_size(path, ec);
	if (ec)
		return IoError { ec.message() };
	if (size != PaletteSize)
		return IoError { "Palette file must be exactly 768 bytes." };
	std::ifstream input;
	input.open(std::string(path).c_str(), std::ios::in | std::ios::binary);
	if (input.fail())
		return IoError { std::string("Failed to open palette file: ")
			                 .append(std::strerror(errno)) };
	input.read(reinterpret_cast<char *>(palette.data()), PaletteSize);
	if (input.fail()) {
		return IoError {
			std::string("Failed to read palette file: ").append(std::strerror(errno))
		};
	}
	input.close();
	if (input.fail()) {
		return IoError {
			std::string("Failed to close palette file: ").append(std::strerror(errno))
		};
	}
	return std::nullopt;
}

/**
 * @brief Loads a built-in palette (default, diablo_menu, hellfire_menu) or a .pal file.
 */
inline std::optional<IoError> LoadPalette(std::string_view nameOrPath, std::array<uint8_t, PaletteSize> &palette)
{
	if (nameOrPath == "default") {
		std::memcpy(palette.data(), dvl_gfx_embedded_default_pal_data, dvl_gfx_embedded_default_pal_size);
	} else if (nameOrPath == "diablo_menu") {
		std::memcpy(palette.data(), dvl_gfx_embedded_diablo_menu_pal_data, dvl_gfx_embedded_diablo_menu_pal_size);
	} else if (nameOrPath == "hellfire_menu") {
		std::memcpy(palette.data(), dvl_gfx_embedded_hellfire_menu_pal_data, dvl_gfx_embedded_hellfire_menu_pal_size);
	} else {
		return LoadPaletteFile(nameOrPath, palette);
	}
	return std::nullopt;
}

} // namespace dvl_gfx
