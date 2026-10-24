#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

#include "kernel/disc.h"
#include "kernel/kernel.h"
#include "kernel/exports.h"

namespace fs = std::filesystem;

namespace kernel {

namespace {

std::mutex g_links_lock;
std::map<std::string, std::string> g_links;    // upper case "\??\D:" -> "\Device\CdRom0"
std::map<std::string, fs::path> g_devices;     // upper case device -> host directory
constexpr char kCdRom[] = "\\DEVICE\\CDROM0";

std::string Upper(std::string s) {
    for (char& c : s) c = char(std::toupper(uint8_t(c)));
    return s;
}

std::string Normalize(std::string p) {
    std::replace(p.begin(), p.end(), '/', '\\');
    std::string out;
    for (char c : p) {
        if (c == '\\' && !out.empty() && out.back() == '\\') continue;
        out += c;
    }
    while (out.size() > 1 && out.back() == '\\') out.pop_back();
    return out;
}

// host lookup that ignores case on file systems that do not
fs::path MatchCase(const fs::path& dir, const std::vector<std::string>& parts) {
    fs::path cur = dir;
    for (auto& part : parts) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            cur = cur.parent_path();
            continue;
        }
        fs::path direct = cur / part;
        std::error_code ec;
        if (fs::exists(direct, ec)) {
            cur = direct;
            continue;
        }
        bool found = false;
        std::string want = Upper(part);
        for (auto& e : fs::directory_iterator(cur, ec)) {
            if (Upper(e.path().filename().string()) == want) {
                cur = e.path();
                found = true;
                break;
            }
        }
        if (!found) cur = direct;
    }
    return cur;
}

struct File : Object {
    fs::path host;
    std::string xbox;
    FILE* fp = nullptr;
    const Disc::Entry* entry = nullptr;  // on the disc image instead of a host path
    bool dir = false;
    uint64_t pos = 0;
    std::vector<fs::directory_entry> entries;
    std::vector<const Disc::Entry*> disc_entries;
    size_t next = 0;
    bool listed = false;
    bool delete_on_close = false;
    ObjectRef completion_port;       // NtSetInformationFile FileCompletionInformation, null when unassociated
    uint32_t completion_key = 0;
    std::mutex io;  // threads sharing a handle: seek and transfer stay together
    File() : Object(ObjType::File) {}
    ~File() override {
        if (fp) std::fclose(fp);
        if (delete_on_close) {
            std::error_code ec;
            fs::remove(host, ec);
        }
    }
};

struct Link : Object {
    std::string target;
    Link() : Object(ObjType::SymLink) {}
};

uint64_t ToFileTime(fs::file_time_type t) {
    using namespace std::chrono;
    auto sys = time_point_cast<nanoseconds>(t - fs::file_time_type::clock::now() + system_clock::now());
    return uint64_t(sys.time_since_epoch().count() / 100 + 116444736000000000ll);
}

void FillNetworkInfo(uint32_t out, uint64_t t, uint64_t size, bool dir) {
    for (int i = 0; i < 4; i++) W64(out + i * 8, t);
    W64(out + 0x20, (size + 0xFFF) & ~0xFFFull);
    W64(out + 0x28, size);
    W32(out + 0x30, dir ? 0x10 : 0x80);  // FILE_ATTRIBUTE_DIRECTORY or NORMAL
}

void FillNetworkInfo(uint32_t out, const fs::path& p) {
    std::error_code ec;
    bool dir = fs::is_directory(p, ec);
    FillNetworkInfo(out, ToFileTime(fs::last_write_time(p, ec)), dir ? 0 : fs::file_size(p, ec), dir);
}

void FillNetworkInfo(uint32_t out, const Disc::Entry& e) {
    FillNetworkInfo(out, MountedDisc()->Time(), e.dir ? 0 : e.size, e.dir);
}

bool WildMatch(const std::string& pat, const std::string& s) {
    size_t p = 0, i = 0, star = std::string::npos, mark = 0;
    while (i < s.size()) {
        if (p < pat.size() && (pat[p] == '?' || std::toupper(uint8_t(pat[p])) == std::toupper(uint8_t(s[i])))) {
            p++;
            i++;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = i;
        } else if (star != std::string::npos) {
            p = star + 1;
            i = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') p++;
    return p == pat.size();
}

// past 2 GB a long offset wraps on Windows
void Seek(FILE* fp, uint64_t pos) {
#ifdef _WIN32
    _fseeki64(fp, int64_t(pos), SEEK_SET);
#else
    fseeko(fp, off_t(pos), SEEK_SET);
#endif
}

void IoStatus(uint32_t iosb, uint32_t status, uint32_t info) {
    if (!iosb) return;
    W32(iosb, status);
    W32(iosb + 4, info);
}

std::shared_ptr<File> FileFromHandle(uint32_t h) {
    ObjectRef o = FromHandle(h);
    if (!o || o->type != ObjType::File) return nullptr;
    return std::static_pointer_cast<File>(o);
}

std::string ObjectName(uint32_t attrs, uint32_t* root = nullptr) {
    if (root) *root = R32(attrs);
    return ReadAnsiString(R32(attrs + 4));
}

void Complete(uint32_t event, uint32_t apc, uint32_t apc_ctx, uint32_t iosb) {
    if (event)
        if (ObjectRef e = FromHandle(event)) SetEvent(*e);
    if (apc && Current()) QueueApc(*Current(), apc, apc_ctx, iosb, 0);
}

}  // namespace

std::string ReadAnsiString(uint32_t str) {
    if (!str) return {};
    uint16_t len = R16(str);
    uint32_t buf = R32(str + 4);
    if (!buf) return {};
    return std::string(reinterpret_cast<const char*>(HostPtr(buf)), len);
}

std::string ReadCString(uint32_t va, size_t max) {
    std::string s;
    if (!va) return s;
    for (size_t i = 0; i < max; i++) {
        char c = char(G<uint8_t>(va + uint32_t(i)));
        if (!c) break;
        s += c;
    }
    return s;
}

void MountDefaults(const std::string& game_dir, const std::string& data_dir) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    fs::path d = data_dir;
    g_devices[kCdRom] = game_dir;
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION1"] = d / "E";
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION2"] = d / "C";
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION3"] = d / "X";
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION4"] = d / "Y";
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION5"] = d / "Z";
    for (auto& [k, v] : g_devices) {
        std::error_code ec;
        fs::create_directories(v, ec);
    }
    g_links["\\??\\D:"] = "\\Device\\CdRom0";
    const Disc* disc = MountedDisc();
    LOGI("files: D: is %s, hard disk under %s", disc ? disc->Path().string().c_str() : game_dir.c_str(), data_dir.c_str());
}

bool CreateSymbolicLink(const std::string& link, const std::string& target) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    g_links[Upper(Normalize(link))] = Normalize(target);
    LOGI("files: link %s -> %s", link.c_str(), target.c_str());
    return true;
}

bool DeleteSymbolicLink(const std::string& link) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    return g_links.erase(Upper(Normalize(link))) != 0;
}

std::string QuerySymbolicLink(const std::string& link) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    auto it = g_links.find(Upper(Normalize(link)));
    return it == g_links.end() ? std::string() : it->second;
}

namespace {

struct Target {
    std::string host;                    // empty on the disc image or unmapped
    bool disc = false;                   // on the mounted disc image
    const Disc::Entry* entry = nullptr;  // there: the file or folder, null when missing
};

Target Resolve(const std::string& xbox_path, uint32_t root_handle) {
    std::string p = Normalize(xbox_path);
    if (root_handle) {
        if (auto f = FileFromHandle(root_handle)) p = Normalize(f->xbox + "\\" + p);
    }
    if (p.size() >= 2 && p[1] == ':') p = "\\??\\" + p;
    std::lock_guard<std::mutex> lock(g_links_lock);
    for (int depth = 0; depth < 8; depth++) {
        std::string up = Upper(p);
        bool changed = false;
        for (auto& [k, v] : g_links) {
            if (up.compare(0, k.size(), k) == 0 && (up.size() == k.size() || up[k.size()] == '\\')) {
                p = v + p.substr(k.size());
                changed = true;
                break;
            }
        }
        if (!changed) break;
    }
    std::string up = Upper(p);
    const std::pair<const std::string, fs::path>* best = nullptr;
    for (auto& e : g_devices)
        if (up.compare(0, e.first.size(), e.first) == 0 && (up.size() == e.first.size() || up[e.first.size()] == '\\'))
            if (!best || e.first.size() > best->first.size()) best = &e;
    if (!best) return {};
    std::vector<std::string> parts;
    std::string rest = p.substr(best->first.size());
    size_t s = 0;
    while (s < rest.size()) {
        size_t e = rest.find('\\', s);
        if (e == std::string::npos) e = rest.size();
        if (e > s) parts.push_back(rest.substr(s, e - s));
        s = e + 1;
    }
    // no Xbox name holds ':', and fs::path would take "D:" as a host drive
    for (auto& part : parts)
        if (part.find(':') != std::string::npos) return {};
    if (best->first == kCdRom)
        if (const Disc* disc = MountedDisc()) return {{}, true, disc->Find(parts)};
    return {MatchCase(best->second, parts).string()};
}

// the disc image is read only: nothing on it is created, replaced or written
uint32_t OpenOnDisc(const Disc::Entry* e, const std::string& name, uint32_t handle_out, uint32_t iosb, uint32_t disposition,
                    uint32_t options) {
    if (!e && (disposition == 1 || disposition == 4)) {  // FILE_OPEN or FILE_OVERWRITE need an existing file
        LOG_FIRST(200, "missing %s", name.c_str());
        IoStatus(iosb, kObjectNameNotFound, 5);
        return kObjectNameNotFound;
    }
    if (!e || (disposition != 1 && disposition != 3)) {
        IoStatus(iosb, kAccessDenied, 0);
        return kAccessDenied;
    }
    if (e->dir && !(options & 1) && (options & 0x40)) {  // options 1 FILE_DIRECTORY_FILE, 0x40 FILE_NON_DIRECTORY_FILE
        IoStatus(iosb, kFileIsADirectory, 0);
        return kFileIsADirectory;
    }
    auto f = std::make_shared<File>();
    f->xbox = Normalize(name);
    f->entry = e;
    f->dir = e->dir;
    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, 1);
    LOG_FIRST(400, "open %s", name.c_str());
    return kSuccess;
}

}  // namespace

std::string ResolvePath(const std::string& xbox_path, uint32_t root_handle) { return Resolve(xbox_path, root_handle).host; }

bool ReadGameFile(const std::string& xbox_path, std::vector<uint8_t>& out) {
    Target t = Resolve(xbox_path, 0);
    if (t.disc) {
        if (!t.entry || t.entry->dir) return false;
        out.resize(t.entry->size);
        return MountedDisc()->Read(*t.entry, 0, out.data(), t.entry->size) == t.entry->size;
    }
    std::ifstream f(t.host, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

// exports

KERNEL_EXPORT(190, NtCreateFile, 9) {
    uint32_t handle_out = a[0], access = a[1], attrs = a[2], iosb = a[3], disposition = a[7], options = a[8];
    (void)access;
    uint32_t root;
    std::string name = ObjectName(attrs, &root);
    Target t = Resolve(name, root);
    if (t.disc) return OpenOnDisc(t.entry, name, handle_out, iosb, disposition, options);
    const std::string& host = t.host;
    if (host.empty()) {
        LOGW("NtCreateFile: no mapping for %s", name.c_str());
        IoStatus(iosb, kObjectPathNotFound, 0);
        return kObjectPathNotFound;
    }
    std::error_code ec;
    bool exists = fs::exists(host, ec);
    bool is_dir = exists && fs::is_directory(host, ec);
    bool want_dir = options & 1;
    uint32_t info = 1;  // FILE_OPENED
    if (want_dir || is_dir) {
        if (!exists) {
            if (disposition == 1 || disposition == 4) {
                IoStatus(iosb, kObjectNameNotFound, 5);
                return kObjectNameNotFound;
            }
            fs::create_directories(host, ec);
            info = 2;
        } else if (disposition == 2) {
            IoStatus(iosb, kObjectNameCollision, 4);
            return kObjectNameCollision;
        }
        if (!want_dir && (options & 0x40)) {
            IoStatus(iosb, kFileIsADirectory, 0);
            return kFileIsADirectory;
        }
        auto f = std::make_shared<File>();
        f->host = host;
        f->xbox = Normalize(name);
        f->dir = true;
        W32(handle_out, NewHandle(f));
        IoStatus(iosb, kSuccess, info);
        return kSuccess;
    }
    const char* mode = nullptr;
    switch (disposition) {
        case 0:  // supersede
        case 5:  // overwrite if
            mode = "w+b";
            info = exists ? 3 : 2;
            break;
        case 1:  // open
            if (!exists) {
                LOG_FIRST(200, "missing %s", name.c_str());
                IoStatus(iosb, kObjectNameNotFound, 5);
                return kObjectNameNotFound;
            }
            mode = "r+b";
            break;
        case 2:  // create
            if (exists) {
                IoStatus(iosb, kObjectNameCollision, 4);
                return kObjectNameCollision;
            }
            mode = "w+b";
            info = 2;
            break;
        case 3:  // open if
            mode = exists ? "r+b" : "w+b";
            info = exists ? 1 : 2;
            break;
        case 4:  // overwrite
            if (!exists) {
                IoStatus(iosb, kObjectNameNotFound, 5);
                return kObjectNameNotFound;
            }
            mode = "w+b";
            info = 3;
            break;
        default:
            return kInvalidParameter;
    }
    // a file in a missing folder fails as on the console: XAPI probes saves with OPEN_ALWAYS
    if (!exists && !fs::is_directory(fs::path(host).parent_path(), ec)) {
        IoStatus(iosb, kObjectPathNotFound, 0);
        return kObjectPathNotFound;
    }
    FILE* fp = std::fopen(host.c_str(), mode);
    if (!fp && std::string(mode) == "r+b") fp = std::fopen(host.c_str(), "rb");  // read only media
    if (!fp) {
        LOGW("NtCreateFile: cannot open %s", host.c_str());
        IoStatus(iosb, kAccessDenied, 0);
        return kAccessDenied;
    }
    auto f = std::make_shared<File>();
    f->host = host;
    f->xbox = Normalize(name);
    f->fp = fp;
    f->delete_on_close = options & 0x1000;  // FILE_DELETE_ON_CLOSE
    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, info);
    LOG_FIRST(400, "open %s", name.c_str());
    return kSuccess;
}

KERNEL_EXPORT(202, NtOpenFile, 6) {
    // same as create with FILE_OPEN
    uint32_t handle_out = a[0], attrs = a[2], iosb = a[3], options = a[5];
    uint32_t root;
    std::string name = ObjectName(attrs, &root);
    Target t = Resolve(name, root);
    if (t.disc) return OpenOnDisc(t.entry, name, handle_out, iosb, 1, options);
    const std::string& host = t.host;
    std::error_code ec;
    if (host.empty() || !fs::exists(host, ec)) {
        LOG_FIRST(200, "missing %s", name.c_str());
        IoStatus(iosb, kObjectNameNotFound, 5);
        return host.empty() ? kObjectPathNotFound : kObjectNameNotFound;
    }
    auto f = std::make_shared<File>();
    f->host = host;
    f->xbox = Normalize(name);
    f->dir = fs::is_directory(host, ec);
    if (!f->dir) {
        f->fp = std::fopen(host.c_str(), "r+b");
        if (!f->fp) f->fp = std::fopen(host.c_str(), "rb");
        if (!f->fp) return kAccessDenied;
    }
    (void)options;
    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, 1);
    return kSuccess;
}

// I/O completes before the call returns: the status block is written, then the file object, the event and the
// APC are signalled (GetOverlappedResult without an event waits on the file handle)
static uint32_t Finish(File& f, uint32_t status, uint32_t n, uint32_t event, uint32_t apc, uint32_t apc_ctx, uint32_t iosb) {
    IoStatus(iosb, status, n);
    SetEvent(f);
    Complete(event, apc, apc_ctx, iosb);
    // a file bound to a completion port posts its packet at completion; transfers never pend here, so the packet
    // is queued synchronously, and an APC routine (which NT forbids beside a port) suppresses the post
    if (f.completion_port && !apc) PostIoCompletion(f.completion_port, f.completion_key, apc_ctx, status, n);
    return status;
}

static uint64_t Offset(File& f, uint32_t off) {
    int64_t o = off ? int64_t(R64(off)) : -1;
    return o >= 0 ? uint64_t(o) : f.pos;
}

// pages is the FILE_SEGMENT_ELEMENT array for scatter and gather, 0 for one buffer
static uint32_t Transfer(const Args& a, bool write, uint32_t pages) {
    uint32_t h = a[0], event = a[1], apc = a[2], apc_ctx = a[3], iosb = a[4], buf = a[5], len = a[6], off = a[7];
    auto f = FileFromHandle(h);
    if (!f || (!f->fp && !f->entry)) return kInvalidHandle;
    if (write && f->entry) return kAccessDenied;
    ResetEvent(*f);
    std::lock_guard<std::mutex> lock(f->io);
    uint64_t pos = Offset(*f, off);
    if (f->fp) Seek(f->fp, pos);
    uint32_t n = 0;
    while (n < len) {
        uint32_t at = pages ? R32(pages + n / 0x1000 * 8) + n % 0x1000 : buf + n;
        uint32_t chunk = pages ? std::min(len - n, 0x1000 - n % 0x1000) : len;
        size_t done = f->entry ? MountedDisc()->Read(*f->entry, pos + n, HostPtr(at), chunk)
                      : write  ? std::fwrite(HostPtr(at), 1, chunk, f->fp)
                               : std::fread(HostPtr(at), 1, chunk, f->fp);
        n += uint32_t(done);
        if (done < chunk) break;
    }
    f->pos = pos + n;
    if (write && n < len) return Finish(*f, 0xC000007F, n, event, apc, apc_ctx, iosb);  // disk full
    if (!write && len && !n) return Finish(*f, kEndOfFile, 0, event, apc, apc_ctx, iosb);
    return Finish(*f, kSuccess, n, event, apc, apc_ctx, iosb);
}

KERNEL_EXPORT(219, NtReadFile, 8) { return Transfer(a, false, 0); }
KERNEL_EXPORT(236, NtWriteFile, 8) { return Transfer(a, true, 0); }
// the buffer argument is an array of one page per element
KERNEL_EXPORT(220, NtReadFileScatter, 8) { return Transfer(a, false, a[5]); }
KERNEL_EXPORT(237, NtWriteFileGather, 8) { return Transfer(a, true, a[5]); }

KERNEL_EXPORT(211, NtQueryInformationFile, 5) {
    uint32_t h = a[0], iosb = a[1], out = a[2], len = a[3], cls = a[4];
    auto f = FileFromHandle(h);
    if (!f) return kInvalidHandle;
    std::error_code ec;
    uint64_t size = f->dir ? 0 : f->entry ? f->entry->size : fs::file_size(f->host, ec);
    if (f->fp) {
        std::fflush(f->fp);
        size = fs::file_size(f->host, ec);
    }
    std::memset(HostPtr(out), 0, len);
    switch (cls) {
        case 4: {  // FileBasicInformation
            uint64_t t = f->entry ? MountedDisc()->Time() : ToFileTime(fs::last_write_time(f->host, ec));
            for (int i = 0; i < 4; i++) W64(out + i * 8, t);
            W32(out + 0x20, f->dir ? 0x10 : 0x80);
            IoStatus(iosb, kSuccess, 0x28);
            return kSuccess;
        }
        case 5:  // FileStandardInformation
            W64(out, (size + 0xFFF) & ~0xFFFull);
            W64(out + 8, size);
            W32(out + 0x10, 1);  // NumberOfLinks
            G<uint8_t>(out + 0x15) = f->dir;  // Directory, after DeletePending
            IoStatus(iosb, kSuccess, 0x18);
            return kSuccess;
        case 14:  // FilePositionInformation
            W64(out, f->pos);
            IoStatus(iosb, kSuccess, 8);
            return kSuccess;
        case 34:  // FileNetworkOpenInformation
            if (f->entry) FillNetworkInfo(out, *f->entry);
            else FillNetworkInfo(out, f->host);
            IoStatus(iosb, kSuccess, 0x38);
            return kSuccess;
        case 22:  // FileModeInformation
        case 16:
            IoStatus(iosb, kSuccess, 4);
            return kSuccess;
        default:
            LOG_FIRST(8, "NtQueryInformationFile: class %u", cls);
            return kInvalidParameter;
    }
}

KERNEL_EXPORT(226, NtSetInformationFile, 5) {
    uint32_t h = a[0], iosb = a[1], in = a[2], len = a[3], cls = a[4];
    auto f = FileFromHandle(h);
    if (!f) return kInvalidHandle;
    if (f->entry && cls != 14 && cls != 4 && cls != 30) return kAccessDenied;
    std::error_code ec;
    switch (cls) {
        case 14:  // position
            f->pos = R64(in);
            break;
        case 19:  // allocation: only a size below the end of file cuts it
        case 20:  // end of file
            if (f->fp) {
                std::lock_guard<std::mutex> lock(f->io);
                std::fflush(f->fp);
                uint64_t size = fs::file_size(f->host, ec);
                if (cls == 20 || R64(in) < size) fs::resize_file(f->host, R64(in), ec);
            }
            break;
        case 13:  // disposition
            f->delete_on_close = G<uint8_t>(in) != 0;
            break;
        case 10: {  // rename: ReplaceIfExists, RootDirectory, FileName (ANSI_STRING inline)
            uint32_t root = R32(in + 4);
            std::string target = ReadAnsiString(in + 8);
            std::string host = ResolvePath(target, root);
            if (host.empty()) return kObjectPathNotFound;
            if (f->fp) std::fflush(f->fp);
            fs::rename(f->host, host, ec);
            if (ec) return kAccessDenied;
            f->host = host;
            break;
        }
        case 4:  // basic, times and attributes are not kept
            break;
        case 30: {  // FileCompletionInformation: port handle, then the key every packet for this file carries
            // D8: Finish reads completion_port/completion_key under f->io (held by
            // Transfer across the whole transfer), so the reset, the assignment and
            // the key all happen under that same mutex; otherwise a bind/unbind on
            // one guest thread races a completing transfer on another on the
            // shared_ptr itself. Lock order matches Transfer: f->io before the
            // handle-table lock FromHandle takes below.
            std::lock_guard<std::mutex> lock(f->io);
            if (len < 8) return kInvalidParameter;
            uint32_t port_handle = R32(in);
            if (!port_handle) {
                f->completion_port.reset();
                f->completion_key = 0;
                break;
            }
            ObjectRef port = FromHandle(port_handle);
            if (!port) return kInvalidHandle;
            // R12: NewIoCompletion is the only constructor of ObjType::Queue, so this tag test and
            // PostIoCompletion's dynamic type test accept exactly the same objects
            if (port->type != ObjType::Queue) return kObjectTypeMismatch;
            f->completion_port = port;
            f->completion_key = R32(in + 4);
            break;
        }
        default:
            LOG_FIRST(8, "NtSetInformationFile: class %u", cls);
            return kInvalidParameter;
    }
    IoStatus(iosb, kSuccess, 0);
    return kSuccess;
}

KERNEL_EXPORT(218, NtQueryVolumeInformationFile, 5) {
    uint32_t iosb = a[1], out = a[2], len = a[3], cls = a[4];
    std::memset(HostPtr(out), 0, len);
    switch (cls) {
        case 3:  // FileFsSizeInformation: 16 GiB total, 8 GiB free in 16 KiB units
            W64(out, 0x100000);
            W64(out + 8, 0x80000);
            W32(out + 0x10, 32);
            W32(out + 0x14, 512);
            IoStatus(iosb, kSuccess, 0x18);
            return kSuccess;
        case 7:  // FileFsFullSizeInformation
            W64(out, 0x100000);
            W64(out + 8, 0x80000);
            W64(out + 0x10, 0x80000);
            W32(out + 0x18, 32);
            W32(out + 0x1C, 512);
            IoStatus(iosb, kSuccess, 0x20);
            return kSuccess;
        case 1:  // FileFsVolumeInformation
            W32(out + 8, 0x12345678);  // VolumeSerialNumber
            IoStatus(iosb, kSuccess, 0x18);
            return kSuccess;
        case 5:  // FileFsAttributeInformation: attributes, max name, name length, "FATX"
            W32(out, 0);
            W32(out + 4, 42);
            W32(out + 8, 4);
            std::memcpy(HostPtr(out + 12), "FATX", 4);
            IoStatus(iosb, kSuccess, 16);
            return kSuccess;
        default:
            LOG_FIRST(8, "NtQueryVolumeInformationFile: class %u", cls);
            return kInvalidParameter;
    }
}

KERNEL_EXPORT(207, NtQueryDirectoryFile, 10) {
    uint32_t h = a[0], event = a[1], apc = a[2], apc_ctx = a[3], iosb = a[4], out = a[5], len = a[6], cls = a[7],
             mask = a[8], restart = a[9];
    auto f = FileFromHandle(h);
    if (!f || !f->dir) return kInvalidHandle;
    if (cls != 1) {
        LOG_FIRST(8, "NtQueryDirectoryFile: class %u", cls);
        return kInvalidParameter;
    }
    std::string pat = mask ? ReadAnsiString(mask) : "*";
    if (!f->listed || (restart & 0xFF)) {
        f->entries.clear();
        f->disc_entries.clear();
        if (f->entry) {
            for (const Disc::Entry& e : f->entry->children)
                if (WildMatch(pat, e.name)) f->disc_entries.push_back(&e);
        } else {
            std::error_code ec;
            for (auto& e : fs::directory_iterator(f->host, ec))
                if (WildMatch(pat, e.path().filename().string())) f->entries.push_back(e);
            std::sort(f->entries.begin(), f->entries.end());
        }
        f->next = 0;
        f->listed = true;
    }
    if (f->next >= (f->entry ? f->disc_entries.size() : f->entries.size())) {
        IoStatus(iosb, kNoMoreFiles, 0);
        return f->next == 0 ? kNoSuchFile : kNoMoreFiles;
    }
    size_t i = f->next++;
    std::string fname = f->entry ? f->disc_entries[i]->name : f->entries[i].path().filename().string();
    uint32_t need = 0x40 + uint32_t(fname.size());
    if (len < need) return kBufferTooSmall;
    std::memset(HostPtr(out), 0, need);
    // times, sizes and attributes sit at the same place
    if (f->entry) FillNetworkInfo(out + 8, *f->disc_entries[i]);
    else FillNetworkInfo(out + 8, f->entries[i].path());
    // FILE_DIRECTORY_INFORMATION has EndOfFile before AllocationSize, the reverse of the network info
    W32(out + 0x38, R32(out + 8 + 0x30));
    uint64_t alloc = R64(out + 8 + 0x20), eof = R64(out + 8 + 0x28);
    W64(out + 0x28, eof);
    W64(out + 0x30, alloc);
    W32(out + 0x3C, uint32_t(fname.size()));
    std::memcpy(HostPtr(out + 0x40), fname.data(), fname.size());
    IoStatus(iosb, kSuccess, need);
    Complete(event, apc, apc_ctx, iosb);
    return kSuccess;
}

KERNEL_EXPORT(210, NtQueryFullAttributesFile, 2) {
    std::string name = ReadAnsiString(R32(a[0] + 4));
    Target t = Resolve(name, R32(a[0]));
    std::error_code ec;
    if (t.disc ? !t.entry : t.host.empty() || !fs::exists(t.host, ec)) {
        LOG_FIRST(200, "missing %s", name.c_str());
        return kObjectNameNotFound;
    }
    if (t.disc) FillNetworkInfo(a[1], *t.entry);
    else FillNetworkInfo(a[1], t.host);
    return kSuccess;
}

KERNEL_EXPORT(195, NtDeleteFile, 1) {
    std::string host = ResolvePath(ReadAnsiString(R32(a[0] + 4)), R32(a[0]));
    std::error_code ec;
    if (host.empty() || !fs::remove(host, ec)) return kObjectNameNotFound;
    return kSuccess;
}

KERNEL_EXPORT(198, NtFlushBuffersFile, 2) {
    if (auto f = FileFromHandle(a[0]))
        if (f->fp) std::fflush(f->fp);
    IoStatus(a[1], kSuccess, 0);
    return kSuccess;
}

KERNEL_EXPORT(200, NtFsControlFile, 10) {
    IoStatus(a[4], kSuccess, 0);
    return kSuccess;
}

// IOCTL_SCSI_PASS_THROUGH_DIRECT: the reply goes to the data buffer the SCSI_PASS_THROUGH_DIRECT names
constexpr uint32_t kScsiPassThroughDirect = 0x4D014;
constexpr uint32_t kSptdTransferLength = 0xC;
constexpr uint32_t kSptdDataBuffer = 0x14;
constexpr uint32_t kSptdCdb = 0x1C;
constexpr uint8_t kModeSense10 = 0x5A;
constexpr uint8_t kXboxSecurityPage = 0x3E;
constexpr uint32_t kSecurityPageBytes = 13;  // mode header, page code and length, then the three bytes XAPI checks

// XAPI starts a title whose media must be an Xbox DVD only once the drive's security page reports an
// authenticated disc (page bytes 2 and 4 set, byte 3 one); the game files stand in for that disc
static void ScsiPassThrough(uint32_t sptd) {
    uint32_t cdb = sptd + kSptdCdb;
    if (G<uint8_t>(cdb) != kModeSense10 || (G<uint8_t>(cdb + 2) & 0x3F) != kXboxSecurityPage) return;
    uint32_t data = R32(sptd + kSptdDataBuffer), n = R32(sptd + kSptdTransferLength);
    if (!data || n < kSecurityPageBytes || n > 0xFF) return;
    std::memset(HostPtr(data), 0, n);
    G<uint8_t>(data + 1) = uint8_t(n - 2);  // mode data length, big endian
    G<uint8_t>(data + 8) = kXboxSecurityPage;
    G<uint8_t>(data + 9) = uint8_t(n - 10);
    G<uint8_t>(data + 10) = 1;
    G<uint8_t>(data + 11) = 1;
    G<uint8_t>(data + 12) = 1;
}

KERNEL_EXPORT(196, NtDeviceIoControlFile, 10) {
    uint32_t iosb = a[4], code = a[5], in = a[6], out = a[8], out_len = a[9];
    LOG_FIRST(16, "NtDeviceIoControlFile: code %08X", code);
    if (out && out_len) std::memset(HostPtr(out), 0, out_len);
    if (code == 0x70000 && out_len >= 0x18) {  // IOCTL_DISK_GET_DRIVE_GEOMETRY
        W64(out, 1000);
        W32(out + 8, 12);
        W32(out + 12, 255);
        W32(out + 16, 63);
        W32(out + 20, 512);
    }
    if (code == kScsiPassThroughDirect && in) ScsiPassThrough(in);
    IoStatus(iosb, kSuccess, out_len);
    return kSuccess;
}

KERNEL_EXPORT(67, IoCreateSymbolicLink, 2) {
    CreateSymbolicLink(ReadAnsiString(a[0]), ReadAnsiString(a[1]));
    return kSuccess;
}

KERNEL_EXPORT(69, IoDeleteSymbolicLink, 1) {
    return DeleteSymbolicLink(ReadAnsiString(a[0])) ? kSuccess : kObjectNameNotFound;
}

KERNEL_EXPORT(203, NtOpenSymbolicLinkObject, 2) {
    std::string name = ObjectName(a[1]);
    std::string target = QuerySymbolicLink(name);
    if (target.empty()) return kObjectNameNotFound;
    auto l = std::make_shared<Link>();
    l->target = target;
    W32(a[0], NewHandle(l));
    return kSuccess;
}

KERNEL_EXPORT(215, NtQuerySymbolicLinkObject, 3) {
    ObjectRef o = FromHandle(a[0]);
    if (!o || o->type != ObjType::SymLink) return kInvalidHandle;
    auto& t = static_cast<Link&>(*o).target;
    uint32_t str = a[1];
    uint16_t maxlen = R16(str + 2);
    if (a[2]) W32(a[2], uint32_t(t.size()));
    if (t.size() > maxlen) return kBufferTooSmall;
    std::memcpy(HostPtr(R32(str + 4)), t.data(), t.size());
    W16(str, uint16_t(t.size()));
    return kSuccess;
}

// the volume behind a file: the mounted disc image, or the host folder standing in for a hard-disk partition
namespace {

struct Volume {
    bool optical = false;
    uint64_t total = 0;
    uint64_t avail = 0;
    uint32_t sectors = 1;
    uint32_t sector_bytes = 2048;
    uint32_t device = 2;         // FILE_DEVICE_CD_ROM, or FILE_DEVICE_DISK below
    uint32_t characteristics = 0x22;  // FILE_READ_ONLY_DEVICE | FILE_DEVICE_IS_MOUNTED
    uint64_t creation = 0;
    const char* fs_name = "XDVDFS";
    uint32_t max_component = 64;
};

Volume VolumeOf(File& f) {
    Volume v;
    std::string up = Upper(f.xbox);
    v.optical = f.entry != nullptr || up.find("CDROM0") != std::string::npos ||
                (up.size() >= 2 && up[0] == 'D' && up[1] == ':');
    std::error_code ec;
    if (v.optical && MountedDisc()) {
        v.total = fs::file_size(MountedDisc()->Path(), ec);
        if (ec) v.total = 0;
        v.avail = 0;
        v.creation = MountedDisc()->Time();
    } else {
        fs::path where = f.host.empty() ? fs::path(".") : fs::path(f.host);
        fs::space_info space = fs::space(where, ec);
        if (!ec) {
            v.total = space.capacity;
            v.avail = space.available;
        }
        v.creation = uint64_t(Now100ns());
        if (!v.optical) {
            v.sectors = 32;
            v.sector_bytes = 512;
            v.device = 7;  // FILE_DEVICE_DISK
            v.characteristics = 0x20;  // FILE_DEVICE_IS_MOUNTED
            v.fs_name = "FATX";
            v.max_component = 42;
        }
    }
    return v;
}

bool TypeAllowsFile(uint32_t object_type) {
    if (!object_type) return true;
    if (object_type == DataAddress(71) || object_type == DataAddress(240)) return true;  // file or directory
    const uint32_t other[] = {DataAddress(64), DataAddress(16), DataAddress(30), DataAddress(22),
                              DataAddress(31), DataAddress(259), DataAddress(249), DataAddress(70)};
    for (uint32_t known : other)
        if (known && object_type == known) return false;
    return true;  // an unrecognised type address is not second-guessed
}

bool TypeMatches(uint32_t object_type, const ObjectRef& obj) {
    if (!object_type) return true;
    ObjType want;
    if (object_type == DataAddress(71) || object_type == DataAddress(240)) want = ObjType::File;
    else if (object_type == DataAddress(64)) want = ObjType::Queue;
    else if (object_type == DataAddress(16)) want = ObjType::Event;
    else if (object_type == DataAddress(30)) want = ObjType::Semaphore;
    else if (object_type == DataAddress(22)) want = ObjType::Mutant;
    else if (object_type == DataAddress(31)) want = ObjType::Timer;
    else if (object_type == DataAddress(259)) want = ObjType::Thread;
    else if (object_type == DataAddress(249)) want = ObjType::SymLink;
    else if (object_type == DataAddress(70)) want = ObjType::Device;
    else {
        LOG_FIRST(8, "ObOpenObjectByName: unknown object type %08X", object_type);
        return true;
    }
    return obj->type == want;
}

}  // namespace

KERNEL_EXPORT(76, IoQueryVolumeInformation, 5) {
    uint32_t cls = a[1], len = a[2], out = a[3], returned = a[4];
    if (returned) W32(returned, 0);
    ObjectRef o = a[0] ? FromPointer(a[0]) : nullptr;
    if (!o) return kInvalidParameter;
    if (o->type != ObjType::File) return kObjectTypeMismatch;
    auto f = std::static_pointer_cast<File>(o);
    if (!out) return kInvalidParameter;
    Volume v = VolumeOf(*f);
    uint64_t unit = uint64_t(v.sectors) * v.sector_bytes;
    uint64_t total_units = unit ? v.total / unit : 0;
    uint64_t avail_units = unit ? v.avail / unit : 0;
    uint32_t serial = uint32_t(total_units) ^ uint32_t(total_units >> 32) ^ uint32_t(v.creation) ^
                      uint32_t(v.creation >> 32);
    if (!serial) serial = 1;
    uint32_t need = 0;
    switch (cls) {
        case 1: need = 18; break;   // FileFsVolumeInformation through its empty label
        case 3: need = 24; break;   // FileFsSizeInformation
        case 4: need = 8; break;    // FileFsDeviceInformation
        case 7: need = 32; break;   // FileFsFullSizeInformation
        case 5: need = 12 + uint32_t(std::strlen(v.fs_name)); break;  // FileFsAttributeInformation
        default:
            LOG_FIRST(8, "IoQueryVolumeInformation: class %u", cls);
            return kInvalidInfoClass;
    }
    if (len < need) {
        if (returned) W32(returned, need);
        return kBufferTooSmall;
    }
    std::memset(HostPtr(out), 0, need);
    switch (cls) {
        case 1:
            W64(out, v.creation);
            W32(out + 8, serial);
            break;  // label length and SupportsObjects stay zero
        case 3:
            W64(out, total_units);
            W64(out + 8, avail_units);
            W32(out + 16, v.sectors);
            W32(out + 20, v.sector_bytes);
            break;
        case 4:
            W32(out, v.device);
            W32(out + 4, v.characteristics);
            break;
        case 7:
            W64(out, total_units);
            W64(out + 8, avail_units);
            W64(out + 16, avail_units);
            W32(out + 24, v.sectors);
            W32(out + 28, v.sector_bytes);
            break;
        case 5:
            W32(out + 4, v.max_component);
            W32(out + 8, uint32_t(std::strlen(v.fs_name)));
            std::memcpy(HostPtr(out + 12), v.fs_name, std::strlen(v.fs_name));
            break;
    }
    if (returned) W32(returned, need);
    return kSuccess;
}

KERNEL_EXPORT(243, ObOpenObjectByName, 4) {
    uint32_t attrs = a[0], object_type = a[1], parse_context = a[2], handle_out = a[3];
    if (!attrs || !handle_out) return kInvalidParameter;
    W32(handle_out, 0);
    if (parse_context) LOG_FIRST(8, "ObOpenObjectByName: parse context %08X ignored", parse_context);
    uint32_t root = R32(attrs);
    std::string name = ReadAnsiString(R32(attrs + 4));
    if (name.empty()) {
        if (!root) return kObjectNameInvalid;
        ObjectRef self = FromHandle(root);
        if (!self) return kInvalidHandle;
        if (!TypeMatches(object_type, self)) return kObjectTypeMismatch;
        W32(handle_out, NewHandle(self));
        return kSuccess;
    }
    if (ObjectRef named = FindNamed(name)) {
        if (!TypeMatches(object_type, named)) return kObjectTypeMismatch;
        W32(handle_out, NewHandle(named));
        return kSuccess;
    }
    if (std::string target = QuerySymbolicLink(name); !target.empty()) {
        auto link = std::make_shared<Link>();
        link->target = target;
        if (!TypeMatches(object_type, link)) return kObjectTypeMismatch;
        W32(handle_out, NewHandle(link));
        return kSuccess;
    }
    if (!TypeAllowsFile(object_type)) return kObjectTypeMismatch;
    Target t = Resolve(name, root);
    if (t.disc) return OpenOnDisc(t.entry, name, handle_out, 0, 1, 0);
    const std::string& host = t.host;
    if (host.empty()) return kObjectPathNotFound;
    std::error_code ec;
    if (!fs::exists(host, ec)) {
        LOG_FIRST(200, "missing %s", name.c_str());
        return kObjectNameNotFound;
    }
    auto f = std::make_shared<File>();
    f->host = host;
    f->xbox = Normalize(name);
    f->dir = fs::is_directory(host, ec);
    if (!f->dir) {
        f->fp = std::fopen(host.c_str(), "r+b");
        if (!f->fp) f->fp = std::fopen(host.c_str(), "rb");
        if (!f->fp) return kAccessDenied;
    }
    W32(handle_out, NewHandle(f));
    return kSuccess;
}

}  // namespace kernel
