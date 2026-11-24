// PNG files of 8-bit RGB or RGBA pictures (png.c), with a deflate encoder of the runtime's own. Any thread; no state.
#pragma once
#include <stddef.h>
#include <stdint.h>

// The whole file for h rows of w pixels, channels (3: RGB, 4: RGBA) bytes per pixel, rows packed. compress 0 keeps the
// pixels in stored deflate blocks, which is fast and about as large as the pixels (the debugging dumps); 1 filters each
// row and codes it with LZ77 and Huffman codes, about as small and as fast as zlib's level 6: some 0.15 s for a 1280x720
// frame and a second or two for 4K (the screenshots). Returns the malloc'd file with its size in *size, NULL when
// memory runs out or the picture is empty or too large for one IDAT.
uint8_t *png_encode(const uint8_t *px, uint32_t w, uint32_t h, int channels, int compress, size_t *size);

// png_encode into the file path (UTF-8, the platform's separators) through plat_file_write_atomic: path holds the whole
// picture or what it held before, never part of one, and a read-only file there stays. 1 on success; on failure one
// rt_log line says why and 0 is returned.
int png_save(const char *path, const uint8_t *px, uint32_t w, uint32_t h, int channels, int compress);
