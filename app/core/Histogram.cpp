#include "Histogram.h"

#include <cstring>

namespace hqcore {

void accumulateRaw12Histogram(Histogram &out, const uint8_t *rows, size_t strideBytes,
			      int width, int height, int rowPeriod, int shift) {
	if (rowPeriod < 2)
		rowPeriod = 2;
	for (int y = 0; y < height; ++y) {
		if (y % rowPeriod >= 2)
			continue;
		const uint8_t *row = rows + static_cast<size_t>(y) * strideBytes;
		for (int x = 0; x < width; ++x) {
			uint16_t v;
			std::memcpy(&v, row + static_cast<size_t>(x) * 2, sizeof(v));
			++out.bins[(v >> shift) & 0x0FFF];
		}
	}
}

} // namespace hqcore
