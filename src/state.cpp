#include "obn/state.hpp"

#include "obn/json_lite.hpp"
#include "obn/log.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>

namespace obn::state {

Store::Store(std::string path) : path_(std::move(path)) {}

void Store::load()
{
    std::lock_guard<std::mutex> lk(mu_);
    remembered_machine_.clear();
    lv_proto_by_dev_.clear();
    if (path_.empty()) return;

    std::ifstream in(path_, std::ios::binary);
    if (!in.good()) return;
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    if (text.empty()) return;

    std::string err;
    auto root = obn::json::parse(text, &err);
    if (!root) {
        OBN_WARN("state: failed to parse %s: %s", path_.c_str(), err.c_str());
        return;
    }
    remembered_machine_ = root->find("selected_machine").as_string();
    for (const auto& [dev_id, proto] : root->find("lv_proto").as_object()) {
        const std::string p = proto.as_string();
        if (!dev_id.empty() && !p.empty()) lv_proto_by_dev_[dev_id] = p;
    }
}

bool Store::persist_locked() const
{
    if (path_.empty()) return true;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path parent = fs::path(path_).parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);

    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.good()) {
            OBN_ERROR("state: open(%s) failed: %s", tmp.c_str(),
                      std::strerror(errno));
            return false;
        }
        std::string body = "{\n  \"selected_machine\": " +
                           obn::json::escape(remembered_machine_);
        if (!lv_proto_by_dev_.empty()) {
            body += ",\n  \"lv_proto\": {";
            const char* sep = "\n    ";
            for (const auto& [dev_id, proto] : lv_proto_by_dev_) {
                body += sep + obn::json::escape(dev_id) + ": " +
                        obn::json::escape(proto);
                sep = ",\n    ";
            }
            body += "\n  }";
        }
        body += "\n}\n";
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        out.flush();
        if (!out.good()) {
            OBN_ERROR("state: partial write on %s", tmp.c_str());
            out.close();
            fs::remove(tmp, ec);
            return false;
        }
    }

#if defined(_WIN32)
    // Remove the destination first; see the rationale in auth.cpp's
    // persist_locked(), which uses the same pattern.
    fs::remove(path_, ec);
#endif
    fs::rename(tmp, path_, ec);
    if (ec) {
        OBN_ERROR("state: rename(%s -> %s) failed: %s",
                  tmp.c_str(), path_.c_str(), ec.message().c_str());
        std::error_code rmec;
        fs::remove(tmp, rmec);
        return false;
    }
    return true;
}

void Store::remember_machine(const std::string& dev_id)
{
    std::lock_guard<std::mutex> lk(mu_);
    // Ignore deselection, and don't rewrite the file for a value we already
    // hold: the slicer re-asserts the current selection on every cloud
    // reconnect, and this runs on its UI thread.
    if (dev_id.empty()) return;
    if (dev_id == remembered_machine_ && persisted_) return;
    remembered_machine_ = dev_id;
    persisted_          = persist_locked();
}

std::string Store::remembered_machine() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return remembered_machine_;
}

void Store::remember_lv_proto(const std::string& dev_id, const std::string& proto)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (dev_id.empty() || proto.empty()) return;
    std::string& slot = lv_proto_by_dev_[dev_id];
    // Reports repeat every second or so; only a change reaches the disk.
    if (slot == proto && persisted_) return;
    slot       = proto;
    persisted_ = persist_locked();
}

std::map<std::string, std::string> Store::remembered_lv_protos() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return lv_proto_by_dev_;
}

} // namespace obn::state
