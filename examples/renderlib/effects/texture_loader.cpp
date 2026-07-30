// PNG/JPG/etc. → RGBA8 decoder. Wraps stb_image (vendored at
// dependencies/stb/stb_image.h, MIT or Public Domain).
//
// stb_image is included in *exactly one* translation unit — this one — by
// defining STB_IMAGE_IMPLEMENTATION here.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "effect_internal.hpp"

#include <cstdio>
#include <string>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
// We only need PNG for the LUT. Disable everything else to keep code size down
// and to avoid pulling in JPEG decoders we don't ship licenses for. stb_image
// itself is permissively licensed but the decoders are large.
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_STDIO  // we open the file ourselves so wide paths work on Windows
#include "stb_image.h"

#include <Windows.h>

namespace uevr::fx::detail {

DecodedImage load_image_rgba8(const wchar_t* path_utf16) {
    DecodedImage out;
    if (path_utf16 == nullptr) return out;

    // Open via Win32 so wide / non-ASCII paths work. stb_image's STBI_NO_STDIO
    // path takes a memory buffer.
    HANDLE h = CreateFileW(path_utf16, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (1LL << 28)) {
        CloseHandle(h);
        return out;
    }

    std::vector<uint8_t> file_bytes(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    BOOL ok = ReadFile(h, file_bytes.data(), static_cast<DWORD>(file_bytes.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok || read != file_bytes.size()) return out;

    int w = 0, h_px = 0, comp = 0;
    stbi_uc* pixels = stbi_load_from_memory(file_bytes.data(), static_cast<int>(file_bytes.size()),
                                            &w, &h_px, &comp, 4);
    if (pixels == nullptr || w <= 0 || h_px <= 0) {
        if (pixels) stbi_image_free(pixels);
        return out;
    }

    out.width  = w;
    out.height = h_px;
    out.rgba8.assign(pixels, pixels + (static_cast<size_t>(w) * h_px * 4));
    stbi_image_free(pixels);
    return out;
}

} // namespace uevr::fx::detail
