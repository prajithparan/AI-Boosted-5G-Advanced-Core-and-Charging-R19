#include "bff.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace oam_gui_bff {

namespace {

namespace fs = std::filesystem;

std::string content_type_for(const std::string& file) {
    const auto ext = fs::path(file).extension().string();
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".js") return "text/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".json") return "application/json";
    if (ext == ".png") return "image/png";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".woff2") return "font/woff2";
    return "application/octet-stream";
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        throw std::runtime_error("oam-gui-bff: cannot read " + p.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

bool is_safe_id(const std::string& id) {
    if (id.empty() || id.size() > 256 || id == "." || id == "..") {
        return false;
    }
    for (const char c : id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '-' || c == '.' || c == '_' || c == '~';
        if (!ok) {
            return false;
        }
    }
    return true;
}

StaticFiles load_static_files(const std::string& dir) {
    StaticFiles files;
    const fs::path root(dir);
    const auto index = root / "index.html";
    if (!fs::is_regular_file(index)) {
        throw std::runtime_error("oam-gui-bff: " + index.string() +
                                 " not found -- build the web app first (gui/web: npm run build)");
    }
    files["index.html"] = StaticFile{content_type_for("index.html"), read_file(index)};
    const auto assets = root / "assets";
    if (fs::is_directory(assets)) {
        for (const auto& entry : fs::directory_iterator(assets)) {
            if (entry.is_regular_file()) {
                const auto name = entry.path().filename().string();
                files["assets/" + name] = StaticFile{content_type_for(name), read_file(entry.path())};
            }
        }
    }
    return files;
}

} // namespace oam_gui_bff
