#include "core/FrameSource.h"
#include "io/SerReader.h"
#include "io/AviReader.h"
#include "io/FitsReader.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

namespace ls {

namespace {
std::string lowerExt(const std::string& path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext;
}
} // namespace

std::unique_ptr<FrameSource> openFrameSource(const std::string& path) {
    std::string ext = lowerExt(path);
    if (ext == "ser") return std::make_unique<SerReader>(path);
    if (ext == "avi") return std::make_unique<AviReader>(path);
    if (ext == "fits" || ext == "fit" || ext == "fts") return std::make_unique<FitsReader>(path);
    throw std::runtime_error("openFrameSource: unrecognized extension for '" + path + "' (expected .ser/.avi/.fits/.fit/.fts)");
}

} // namespace ls
