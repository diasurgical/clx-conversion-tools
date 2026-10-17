#include "pcx.hpp"

#include <cerrno>
#include <cstring>

#include <dvl_gfx_endian.hpp>

namespace dvl_gfx {

const uint8_t *LoadPcxMeta(const uint8_t *data, int &width, int &height, uint8_t &bpp,
    uint8_t &numPlanes, unsigned &bytesPerLine)
{
	PCXHeader pcxhdr;
	std::memcpy(&pcxhdr, data, PcxHeaderSize);
	width = SwapLE(pcxhdr.xmax) - SwapLE(pcxhdr.xmin) + 1;
	height = SwapLE(pcxhdr.ymax) - SwapLE(pcxhdr.ymin) + 1;
	bpp = pcxhdr.bitsPerPixel;
	numPlanes = pcxhdr.nPlanes;
	bytesPerLine = SwapLE(pcxhdr.bytesPerLine);
	return data + PcxHeaderSize;
}

} // namespace dvl_gfx
