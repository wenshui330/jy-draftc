// jy-draftc
// Copyright (c) 2026 wenshui330
// SPDX-License-Identifier: MIT

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

struct MsvcString {
    union {
        char small[16];
        char *ptr;
    } data{};
    unsigned long long size = 0;
    unsigned long long capacity = 15;
};

using DecryptFn = MsvcString *(*)(void *, MsvcString *, const MsvcString *, const MsvcString *, bool *);
using EncryptFn = MsvcString *(*)(void *, MsvcString *, const MsvcString *);
using EnableFn = void (*)(void *, bool);

struct Error : std::runtime_error {
    int code;
    Error(int c, const std::string &s) : std::runtime_error(s), code(c) {}
};


static constexpr const char *kDec =
    "?decrypt@EncryptUtils@lvve@@QEAA?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@AEBV34@0AEA_N@Z";
static constexpr const char *kEnc =
    "?encrypt@EncryptUtils@lvve@@QEAA?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@AEBV34@@Z";
static constexpr const char *kEnable = "?enable@EncryptUtils@lvve@@QEAAX_N@Z";

static bool g_debug_enabled = false;
static std::mutex g_debug_mutex;

static std::string narrow(const std::wstring &s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

static const char *bool_text(bool value) {
    return value ? "true" : "false";
}

static std::string path_text(const fs::path &path) {
    return narrow(path.wstring());
}

static void debug_log(const std::string &message) {
    if (!g_debug_enabled) return;
    std::ostringstream tid;
    tid << std::this_thread::get_id();
    std::lock_guard<std::mutex> lock(g_debug_mutex);
    std::cerr << "[debug] tid=" << tid.str() << " " << message << "\n";
}

template <class S>
static S trim(S s) {
    auto ws = [](auto c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && ws(s.front())) s.erase(s.begin());
    while (!s.empty() && ws(s.back())) s.pop_back();
    return s;
}

static std::wstring unquote(std::wstring s) {
    s = trim(std::move(s));
    return s.size() >= 2 && s.front() == L'"' && s.back() == L'"' ? s.substr(1, s.size() - 2) : s;
}

static std::string env_unquote(std::string s) {
    s = trim(std::move(s));
    if (s.size() < 2) return s;
    char a = s.front(), b = s.back();
    return ((a == '"' && b == '"') || (a == '\'' && b == '\'')) ? s.substr(1, s.size() - 2) : s;
}

static std::string strip_utf8_bom(std::string s) {
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
    return s;
}

static fs::path exe_dir() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (!n || n >= buf.size()) {
        debug_log("GetModuleFileNameW failed or truncated, using current_path");
        return fs::current_path();
    }
    buf.resize(n);
    fs::path dir = fs::path(buf).parent_path();
    debug_log("exe_dir=" + path_text(dir));
    return dir;
}

static std::string read_all(const fs::path &p) {
    debug_log("read_all begin path=" + path_text(p));
    std::ifstream f(p, std::ios::binary);
    if (!f) throw Error(2, "cannot open input: " + narrow(p.wstring()));
    std::string data{std::istreambuf_iterator<char>(f), {}};
    debug_log("read_all ok path=" + path_text(p) + " bytes=" + std::to_string(data.size()));
    return data;
}

static void write_all(const fs::path &p, const std::string &s) {
    debug_log("write_all begin path=" + path_text(p) + " bytes=" + std::to_string(s.size()));
    std::ofstream f(p, std::ios::binary);
    if (!f) throw Error(4, "cannot open output: " + narrow(p.wstring()));
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
    if (!f) throw Error(4, "failed writing output: " + narrow(p.wstring()));
    debug_log("write_all ok path=" + path_text(p) + " bytes=" + std::to_string(s.size()));
}

static bool to_wide(const std::string &s, std::wstring &out) {
    for (UINT cp : {CP_UTF8, CP_ACP}) {
        int n = MultiByteToWideChar(cp, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (n <= 0) continue;
        out.assign(static_cast<size_t>(n), L'\0');
        return MultiByteToWideChar(cp, 0, s.data(), static_cast<int>(s.size()), out.data(), n) > 0;
    }
    return s.empty() ? (out.clear(), true) : false;
}

static bool parse_install_dir_from_env_text(const std::string &env_text, fs::path *install_dir, std::string *err) {
    debug_log("parse .env begin bytes=" + std::to_string(env_text.size()));
    std::istringstream lines(env_text);
    std::string line;
    size_t line_no = 0;
    while (std::getline(lines, line)) {
        ++line_no;
        line = trim(std::move(line));
        bool had_bom = line.size() >= 3 &&
                       static_cast<unsigned char>(line[0]) == 0xEF &&
                       static_cast<unsigned char>(line[1]) == 0xBB &&
                       static_cast<unsigned char>(line[2]) == 0xBF;
        line = strip_utf8_bom(std::move(line));
        if (had_bom) debug_log(".env line=" + std::to_string(line_no) + " stripped UTF-8 BOM");
        if (line.empty()) {
            debug_log(".env line=" + std::to_string(line_no) + " skipped empty");
            continue;
        }
        if (line[0] == '#') {
            debug_log(".env line=" + std::to_string(line_no) + " skipped comment");
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            debug_log(".env line=" + std::to_string(line_no) + " skipped no '='");
            continue;
        }
        std::string key = trim(line.substr(0, eq));
        if (key != "JY_INSTALL_DIR") {
            debug_log(".env line=" + std::to_string(line_no) + " skipped key=" + key);
            continue;
        }
        debug_log(".env line=" + std::to_string(line_no) + " found JY_INSTALL_DIR");
        std::wstring w;
        if (!to_wide(env_unquote(line.substr(eq + 1)), w)) {
            *err = "JY_INSTALL_DIR is not valid UTF-8/ANSI text";
            debug_log("parse .env failed: " + *err);
            return false;
        }
        fs::path dir(w), dll = dir / L"videoeditor.dll";
        if (dir.empty()) {
            *err = "JY_INSTALL_DIR is empty in .env";
            debug_log("parse .env failed: " + *err);
            return false;
        }
        if (!fs::exists(dir)) {
            *err = "JY_INSTALL_DIR does not exist: " + narrow(dir.wstring());
            debug_log("parse .env failed: " + *err);
            return false;
        }
        if (!fs::exists(dll)) {
            *err = "videoeditor.dll not found under JY_INSTALL_DIR: " + narrow(dir.wstring());
            debug_log("parse .env failed: " + *err);
            return false;
        }
        *install_dir = dir;
        debug_log("parse .env ok install_dir=" + path_text(dir) + " videoeditor=" + path_text(dll));
        return true;
    }

    *err = "JY_INSTALL_DIR missing in .env";
    debug_log("parse .env failed: " + *err);
    return false;
}

static fs::path install_dir_from_env() {
    fs::path env = exe_dir() / L".env";
    debug_log("install_dir_from_env path=" + path_text(env));
    std::string env_text;
    try {
        env_text = read_all(env);
    } catch (...) {
        throw Error(1, "cannot read .env: " + narrow(env.wstring()));
    }

    fs::path dir;
    std::string err;
    if (!parse_install_dir_from_env_text(env_text, &dir, &err)) {
        throw Error(1, err);
    }
    debug_log("install_dir_from_env ok dir=" + path_text(dir));
    return dir;
}

static std::wstring get_env_var(const wchar_t *name) {
    DWORD need = GetEnvironmentVariableW(name, nullptr, 0);
    if (need == 0) return {};
    std::wstring value(need, L'\0');
    DWORD got = GetEnvironmentVariableW(name, value.data(), need);
    if (got == 0 || got >= need) return {};
    value.resize(got);
    return value;
}

static std::wstring path_with_prepended_dir(const fs::path &dir, const std::wstring &original_path) {
    std::wstring result = dir.wstring();
    if (!original_path.empty()) {
        result += L";";
        result += original_path;
    }
    return result;
}

static void configure_dll_search(const fs::path &dir) {
    debug_log("configure_dll_search begin dir=" + path_text(dir));
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    std::wstring original_path = get_env_var(L"PATH");
    std::wstring merged_path = path_with_prepended_dir(dir, original_path);
    debug_log("PATH original_chars=" + std::to_string(original_path.size()) +
              " merged_chars=" + std::to_string(merged_path.size()));
    if (!SetEnvironmentVariableW(L"PATH", merged_path.c_str())) {
        throw Error(2, "SetEnvironmentVariableW(PATH) failed, gle=" + std::to_string(GetLastError()));
    }
    debug_log("SetEnvironmentVariableW(PATH) ok");
    if (!SetCurrentDirectoryW(dir.wstring().c_str())) {
        throw Error(2, "SetCurrentDirectoryW failed, gle=" + std::to_string(GetLastError()) + ", dir=" + path_text(dir));
    }
    debug_log("SetCurrentDirectoryW ok dir=" + path_text(dir));
    if (!SetDllDirectoryW(L"")) {
        throw Error(2, "SetDllDirectoryW failed, gle=" + std::to_string(GetLastError()));
    }
    debug_log("SetDllDirectoryW ok");

    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS)) {
        DWORD gle = GetLastError();
        if (gle != ERROR_PROC_NOT_FOUND && gle != ERROR_INVALID_PARAMETER) {
            throw Error(2, "SetDefaultDllDirectories failed, gle=" + std::to_string(gle));
        }
        debug_log("SetDefaultDllDirectories unavailable or ignored gle=" + std::to_string(gle));
    } else {
        debug_log("SetDefaultDllDirectories ok");
    }

    DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(dir.wstring().c_str());
    if (!cookie) {
        DWORD gle = GetLastError();
        if (gle != ERROR_PROC_NOT_FOUND && gle != ERROR_INVALID_PARAMETER) {
            throw Error(2, "AddDllDirectory failed, gle=" + std::to_string(gle) + ", dir=" + narrow(dir.wstring()));
        }
        debug_log("AddDllDirectory unavailable or ignored gle=" + std::to_string(gle));
    } else {
        debug_log("AddDllDirectory ok dir=" + path_text(dir));
    }
}

static fs::path absolute_from(const fs::path &base, const fs::path &path) {
    if (path.empty() || path.is_absolute()) {
        debug_log("absolute_from input=" + path_text(path) + " result=" + path_text(path));
        return path;
    }
    fs::path result = (base / path).lexically_normal();
    debug_log("absolute_from base=" + path_text(base) + " input=" + path_text(path) + " result=" + path_text(result));
    return result;
}

struct StrArg {

    std::string storage;
    MsvcString s;
    explicit StrArg(std::string v) : storage(std::move(v)) {
        s.size = storage.size();
        if (storage.size() < 16) {
            memset(s.data.small, 0, sizeof(s.data.small));
            memcpy(s.data.small, storage.data(), storage.size());
        } else {
            storage.push_back('\0');
            storage.pop_back();
            s.capacity = storage.size();
            s.data.ptr = storage.data();
        }
        debug_log("MsvcString prepared size=" + std::to_string(s.size) +
                  " capacity=" + std::to_string(s.capacity) +
                  " small=" + bool_text(s.capacity < 16));
    }
};

static std::string take(const MsvcString &s) {
    const char *p = s.capacity < 16 ? s.data.small : s.data.ptr;
    bool valid = p && s.size <= (1ull << 32);
    debug_log("MsvcString take size=" + std::to_string(s.size) +
              " capacity=" + std::to_string(s.capacity) +
              " valid=" + bool_text(valid));
    return valid ? std::string(p, p + s.size) : std::string();
}

struct VeApi {
    // 加载 videoeditor.dll，并缓存加解密入口
    HMODULE dll = nullptr;
    DecryptFn dec = nullptr;
    EncryptFn enc = nullptr;
    EnableFn enable = nullptr;

    template <class T>
    static T sym(HMODULE dll, const char *name, const char *label) {
        debug_log(std::string("GetProcAddress begin label=") + label);
        auto p = GetProcAddress(dll, name);
        if (!p) throw Error(3, std::string(label) + " export missing");
        T out{};
        static_assert(sizeof(out) == sizeof(p), "function pointer size mismatch");
        memcpy(&out, &p, sizeof(out));
        std::ostringstream oss;
        oss << "GetProcAddress ok label=" << label << " address=" << reinterpret_cast<void *>(p);
        debug_log(oss.str());
        return out;
    }

    static VeApi load(const fs::path &dir) {
        debug_log("VeApi::load begin dir=" + path_text(dir));
        configure_dll_search(dir);

        fs::path p = dir / L"videoeditor.dll";
        DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                      LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS;
        debug_log("LoadLibraryExW begin path=" + path_text(p) + " flags=" + std::to_string(flags));
        HMODULE h = LoadLibraryExW(p.wstring().c_str(), nullptr, flags);
        if (!h) {
            throw Error(2, "LoadLibraryExW(videoeditor.dll) failed, gle=" + std::to_string(GetLastError()) +
                               ", path=" + narrow(p.wstring()) +
                               ". Check that JY_INSTALL_DIR points to the version directory that contains videoeditor.dll.");
        }
        std::ostringstream oss;
        oss << "LoadLibraryExW ok handle=" << reinterpret_cast<void *>(h);
        debug_log(oss.str());
        return {h, sym<DecryptFn>(h, kDec, "decrypt"), sym<EncryptFn>(h, kEnc, "encrypt"), sym<EnableFn>(h, kEnable, "enable")};
    }

    std::string decrypt(const std::string &text, bool *ok = nullptr) const {
        debug_log("decrypt begin input_len=" + std::to_string(text.size()));
        StrArg in(text), param("{}");
        MsvcString out{};
        bool good = false;
        dec(nullptr, &out, &in.s, &param.s, &good);
        if (ok) *ok = good;
        std::string result = take(out);
        debug_log("decrypt end ok_flag=" + std::string(bool_text(good)) +
                  " output_len=" + std::to_string(result.size()));
        return result;
    }

    std::string encrypt(const std::string &text) const {
        // 打开 DLL 内部加密开关
        debug_log("encrypt enable begin");
        enable(nullptr, true);
        debug_log("encrypt enable ok");
        debug_log("encrypt begin input_len=" + std::to_string(text.size()));
        StrArg in(text);
        MsvcString out{};
        enc(nullptr, &out, &in.s);
        std::string result = take(out);
        debug_log("encrypt end output_len=" + std::to_string(result.size()));
        return result;
    }
};

struct Job {
    size_t i;
    fs::path in, out;
};

struct Result {
    size_t i = 0;
    int code = 0;
    std::string msg;
};

static void usage() {
    std::cout
        << "usage:\n"
        << "  jy-draftc [--debug] --dec|-d <encrypted-json-file> [output-json-file]\n"
        << "  jy-draftc [--debug] --enc|-e <plaintext-json-file> [output-encrypted-file]\n"
        << "  jy-draftc [--debug] --dec|-d \"file1\",\"file2\",\"file3\"\n"
        << "  jy-draftc [--debug] --enc|-e \"file1\",\"file2\",\"file3\"\n\n"
        << "Options:\n"
        << "  --debug writes detailed diagnostics to stderr\n\n"
        << "Defaults:\n"
        << "  --dec writes <input>.dec.json\n"
        << "  --enc writes <input>.enc.json\n"
        << "  multi-file mode always uses default output paths\n";
}

static std::vector<fs::path> parse_inputs(int argc, wchar_t **argv) {
    debug_log("parse_inputs begin argc=" + std::to_string(argc));
    std::wstring s;
    for (int i = 2; i < argc; ++i) s += (s.empty() ? L"" : L" ") + std::wstring(argv[i]);
    debug_log("parse_inputs combined=" + narrow(s));
    std::vector<fs::path> out;
    std::wstring cur;
    bool q = false;
    for (wchar_t c : s) {
        if (c == L'"') q = !q;
        if (c == L',' && !q) {
            if (auto v = unquote(cur); !v.empty()) out.emplace_back(v);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (auto v = unquote(cur); !v.empty()) out.emplace_back(v);
    debug_log("parse_inputs count=" + std::to_string(out.size()));
    for (size_t i = 0; i < out.size(); ++i) {
        debug_log("parse_inputs item[" + std::to_string(i) + "]=" + path_text(out[i]));
    }
    return out;
}

static size_t workers_for(size_t files) {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    bool mem_ok = GlobalMemoryStatusEx(&m);
    size_t by_mem = mem_ok ? static_cast<size_t>(std::max<unsigned long long>(1, m.ullAvailPhys / (128ull << 20))) : 1;
    size_t by_cpu = std::max(1u, std::thread::hardware_concurrency());
    size_t workers = std::max<size_t>(1, std::min({files, by_cpu, by_mem, static_cast<size_t>(8)}));
    debug_log("workers_for files=" + std::to_string(files) +
              " hardware_concurrency=" + std::to_string(by_cpu) +
              " mem_ok=" + bool_text(mem_ok) +
              " avail_phys=" + std::to_string(mem_ok ? m.ullAvailPhys : 0) +
              " by_mem=" + std::to_string(by_mem) +
              " workers=" + std::to_string(workers));
    return workers;
}

static int run_one(const VeApi &ve, bool enc_mode, const Job &j, std::string &msg) {
    try {
        debug_log("job begin index=" + std::to_string(j.i) +
                  " mode=" + (enc_mode ? "encrypt" : "decrypt") +
                  " input=" + path_text(j.in) +
                  " output=" + path_text(j.out));
        std::string in = read_all(j.in), out;
        if (enc_mode) {
            debug_log("job encrypt step input_len=" + std::to_string(in.size()));
            out = ve.encrypt(in);
            bool ok = false;
            debug_log("job encrypt validation decrypt begin encrypted_len=" + std::to_string(out.size()));
            std::string roundtrip = ve.decrypt(out, &ok);
            if (out.empty()) throw Error(10, "encrypt failed: output_len=0");
            if (!ok || roundtrip != in) {
                std::ostringstream e;
                e << "encrypt validation failed: ok_flag=" << (ok ? "true" : "false")
                  << ", encrypted_len=" << out.size() << ", roundtrip_len=" << roundtrip.size();
                throw Error(11, e.str());
            }
            debug_log("job encrypt validation ok roundtrip_len=" + std::to_string(roundtrip.size()));
        } else {
            bool ok = false;
            debug_log("job decrypt step input_len=" + std::to_string(in.size()));
            out = ve.decrypt(in, &ok);
            if (!ok || out.empty()) {
                std::ostringstream e;
                e << "decrypt failed: ok_flag=" << (ok ? "true" : "false") << ", output_len=" << out.size();
                throw Error(10, e.str());
            }
            debug_log("job decrypt ok output_len=" + std::to_string(out.size()));
        }
        write_all(j.out, out);
        std::ostringstream ok;
        ok << "ok input=" << narrow(j.in.wstring()) << " output=" << narrow(j.out.wstring())
           << " input_len=" << in.size() << " output_len=" << out.size();
        if (enc_mode) ok << " roundtrip_ok=true";
        msg = ok.str();
        debug_log("job end index=" + std::to_string(j.i) + " code=0 message=" + msg);
        return 0;
    } catch (const Error &e) {
        std::ostringstream fail;
        fail << "input=" << narrow(j.in.wstring()) << " output=" << narrow(j.out.wstring()) << " " << e.what();
        msg = fail.str();
        debug_log("job end index=" + std::to_string(j.i) + " code=" + std::to_string(e.code) + " message=" + msg);
        return e.code;
    } catch (const std::exception &e) {
        std::ostringstream fail;
        fail << "input=" << narrow(j.in.wstring()) << " output=" << narrow(j.out.wstring()) << " " << e.what();
        msg = fail.str();
        debug_log("job end index=" + std::to_string(j.i) + " code=1 message=" + msg);
        return 1;
    }
}

static int run_jobs(const VeApi &ve, bool enc_mode, const std::vector<Job> &jobs) {

    size_t n = workers_for(jobs.size()), ok = 0;
    debug_log("run_jobs begin total=" + std::to_string(jobs.size()) +
              " mode=" + (enc_mode ? "encrypt" : "decrypt") +
              " workers=" + std::to_string(n));
    std::atomic_size_t next{0};
    std::vector<Result> results(jobs.size());
    std::vector<std::thread> ts;
    ts.reserve(n);
    for (size_t t = 0; t < n; ++t) {
        ts.emplace_back([&, t] {
            debug_log("worker start worker_index=" + std::to_string(t));
            for (;;) {
                size_t i = next.fetch_add(1);
                if (i >= jobs.size()) {
                    debug_log("worker exit worker_index=" + std::to_string(t));
                    return;
                }
                debug_log("worker picked worker_index=" + std::to_string(t) + " job_index=" + std::to_string(i));
                std::string msg;
                int code = run_one(ve, enc_mode, jobs[i], msg);
                results[i] = {jobs[i].i, code, std::move(msg)};
            }
        });
    }
    for (auto &t : ts) t.join();
    for (const auto &r : results) {
        (r.code ? std::cerr : std::cout) << "[" << r.i + 1 << "/" << jobs.size() << "] "
                                         << (r.code ? "failed code=" + std::to_string(r.code) + " " : "")
                                         << r.msg << "\n";
        ok += r.code == 0;
    }
    std::cout << "summary total=" << jobs.size() << " ok=" << ok << " failed=" << jobs.size() - ok << " workers=" << n << "\n";
    debug_log("run_jobs end total=" + std::to_string(jobs.size()) +
              " ok=" + std::to_string(ok) +
              " failed=" + std::to_string(jobs.size() - ok));
    return ok == jobs.size() ? 0 : 20;
}

int wmain(int argc, wchar_t **argv) {
    try {
        bool debug_requested = false;
        int raw_argc = argc;
        std::vector<std::wstring> arg_storage;
        arg_storage.reserve(static_cast<size_t>(argc));
        for (int i = 0; i < argc; ++i) {
            std::wstring arg(argv[i]);
            if (i > 0 && arg == L"--debug") {
                debug_requested = true;
                continue;
            }
            arg_storage.push_back(std::move(arg));
        }

        g_debug_enabled = debug_requested;
        std::vector<wchar_t *> effective_argv;
        effective_argv.reserve(arg_storage.size());
        for (auto &arg : arg_storage) effective_argv.push_back(arg.data());
        argc = static_cast<int>(effective_argv.size());
        argv = effective_argv.data();

        debug_log("debug enabled raw_argc=" + std::to_string(raw_argc) +
                  " effective_argc=" + std::to_string(argc));
        for (int i = 0; i < argc; ++i) {
            debug_log("argv[" + std::to_string(i) + "]=" + narrow(std::wstring(argv[i])));
        }

        if (argc < 3) return usage(), 64;
        std::wstring mode(argv[1]);
        bool dec = mode == L"--dec" || mode == L"-d", enc = mode == L"--enc" || mode == L"-e";
        if (!dec && !enc) return usage(), 64;
        debug_log("mode=" + narrow(mode) + " dec=" + bool_text(dec) + " enc=" + bool_text(enc));

        bool comma = std::any_of(argv + 2, argv + argc, [](wchar_t *s) { return std::wstring(s).find(L',') != std::wstring::npos; });
        debug_log(std::string("comma_mode=") + bool_text(comma));
        std::vector<fs::path> inputs = comma ? parse_inputs(argc, argv) : std::vector<fs::path>{unquote(argv[2])};
        if (inputs.empty()) return usage(), 64;
        if (!comma && argc > 4) return std::cerr << "too many arguments for single-file mode\n", 64;
        if (comma && inputs.size() <= 1 && argc > 3) return std::cerr << "comma-list mode does not accept a separate output path\n", 64;

        fs::path launch_cwd = fs::current_path();
        debug_log("launch_cwd=" + path_text(launch_cwd));
        std::vector<Job> jobs;
        jobs.reserve(inputs.size());
        for (size_t i = 0; i < inputs.size(); ++i) {
            fs::path input = absolute_from(launch_cwd, inputs[i]);
            fs::path out = !comma && inputs.size() == 1 && argc >= 4
                               ? absolute_from(launch_cwd, fs::path(argv[3]))
                               : input.parent_path() / (input.filename().wstring() + (dec ? L".dec.json" : L".enc.json"));
            jobs.push_back({i, input, out});
            debug_log("planned job index=" + std::to_string(i) +
                      " input=" + path_text(input) +
                      " output=" + path_text(out));
        }

        VeApi ve = VeApi::load(install_dir_from_env());
        return run_jobs(ve, enc, jobs);
    } catch (const Error &e) {
        debug_log(std::string("fatal Error code=") + std::to_string(e.code) + " message=" + e.what());
        std::cerr << e.what() << "\n";
        std::cerr << "Create .env next to jy-draftc.exe with: JY_INSTALL_DIR=E:\\JianyingPro\\10.6.5.14040\n";
        return e.code;
    } catch (const std::exception &e) {
        debug_log(std::string("fatal std::exception message=") + e.what());
        std::cerr << e.what() << "\n";
        std::cerr << "Create .env next to jy-draftc.exe with: JY_INSTALL_DIR=E:\\JianyingPro\\10.6.5.14040\n";
        return 1;
    }
}
