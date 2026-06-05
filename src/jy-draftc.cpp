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

static std::string narrow(const std::wstring &s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
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
    if (!n || n >= buf.size()) return fs::current_path();
    buf.resize(n);
    return fs::path(buf).parent_path();
}

static std::string read_all(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw Error(2, "cannot open input: " + narrow(p.wstring()));
    return {std::istreambuf_iterator<char>(f), {}};
}

static void write_all(const fs::path &p, const std::string &s) {
    std::ofstream f(p, std::ios::binary);
    if (!f) throw Error(4, "cannot open output: " + narrow(p.wstring()));
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
    if (!f) throw Error(4, "failed writing output: " + narrow(p.wstring()));
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
    std::istringstream lines(env_text);
    std::string line;
    while (std::getline(lines, line)) {
        line = strip_utf8_bom(trim(std::move(line)));
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos || trim(line.substr(0, eq)) != "JY_INSTALL_DIR") continue;
        std::wstring w;
        if (!to_wide(env_unquote(line.substr(eq + 1)), w)) {
            *err = "JY_INSTALL_DIR is not valid UTF-8/ANSI text";
            return false;
        }
        fs::path dir(w), dll = dir / L"videoeditor.dll";
        if (dir.empty()) {
            *err = "JY_INSTALL_DIR is empty in .env";
            return false;
        }
        if (!fs::exists(dir)) {
            *err = "JY_INSTALL_DIR does not exist: " + narrow(dir.wstring());
            return false;
        }
        if (!fs::exists(dll)) {
            *err = "videoeditor.dll not found under JY_INSTALL_DIR: " + narrow(dir.wstring());
            return false;
        }
        *install_dir = dir;
        return true;
    }

    *err = "JY_INSTALL_DIR missing in .env";
    return false;
}

static fs::path install_dir_from_env() {
    fs::path env = exe_dir() / L".env";
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
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    if (!SetEnvironmentVariableW(L"PATH", path_with_prepended_dir(dir, get_env_var(L"PATH")).c_str())) {
        throw Error(2, "SetEnvironmentVariableW(PATH) failed, gle=" + std::to_string(GetLastError()));
    }
    SetCurrentDirectoryW(dir.wstring().c_str());
    SetDllDirectoryW(L"");

    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS)) {
        DWORD gle = GetLastError();
        if (gle != ERROR_PROC_NOT_FOUND && gle != ERROR_INVALID_PARAMETER) {
            throw Error(2, "SetDefaultDllDirectories failed, gle=" + std::to_string(gle));
        }
    }

    DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(dir.wstring().c_str());
    if (!cookie) {
        DWORD gle = GetLastError();
        if (gle != ERROR_PROC_NOT_FOUND && gle != ERROR_INVALID_PARAMETER) {
            throw Error(2, "AddDllDirectory failed, gle=" + std::to_string(gle) + ", dir=" + narrow(dir.wstring()));
        }
    }
}

static fs::path absolute_from(const fs::path &base, const fs::path &path) {
    if (path.empty() || path.is_absolute()) return path;
    return (base / path).lexically_normal();
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
    }
};

static std::string take(const MsvcString &s) {
    const char *p = s.capacity < 16 ? s.data.small : s.data.ptr;
    return p && s.size <= (1ull << 32) ? std::string(p, p + s.size) : std::string();
}

struct VeApi {
    // 加载 videoeditor.dll，并缓存加解密入口
    HMODULE dll = nullptr;
    DecryptFn dec = nullptr;
    EncryptFn enc = nullptr;
    EnableFn enable = nullptr;

    template <class T>
    static T sym(HMODULE dll, const char *name, const char *label) {
        auto p = GetProcAddress(dll, name);
        if (!p) throw Error(3, std::string(label) + " export missing");
        T out{};
        static_assert(sizeof(out) == sizeof(p), "function pointer size mismatch");
        memcpy(&out, &p, sizeof(out));
        return out;
    }

    static VeApi load(const fs::path &dir) {
        configure_dll_search(dir);

        fs::path p = dir / L"videoeditor.dll";
        DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                      LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS;
        HMODULE h = LoadLibraryExW(p.wstring().c_str(), nullptr, flags);
        if (!h) {
            throw Error(2, "LoadLibraryExW(videoeditor.dll) failed, gle=" + std::to_string(GetLastError()) +
                               ", path=" + narrow(p.wstring()) +
                               ". Check that JY_INSTALL_DIR points to the version directory that contains videoeditor.dll.");
        }
        return {h, sym<DecryptFn>(h, kDec, "decrypt"), sym<EncryptFn>(h, kEnc, "encrypt"), sym<EnableFn>(h, kEnable, "enable")};
    }

    std::string decrypt(const std::string &text, bool *ok = nullptr) const {
        StrArg in(text), param("{}");
        MsvcString out{};
        bool good = false;
        dec(nullptr, &out, &in.s, &param.s, &good);
        if (ok) *ok = good;
        return take(out);
    }

    std::string encrypt(const std::string &text) const {
        // 打开 DLL 内部加密开关
        enable(nullptr, true);
        StrArg in(text);
        MsvcString out{};
        enc(nullptr, &out, &in.s);
        return take(out);
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
        << "  jy-draftc --dec|-d <encrypted-json-file> [output-json-file]\n"
        << "  jy-draftc --enc|-e <plaintext-json-file> [output-encrypted-file]\n"
        << "  jy-draftc --dec|-d \"file1\",\"file2\",\"file3\"\n"
        << "  jy-draftc --enc|-e \"file1\",\"file2\",\"file3\"\n\n"
        << "Defaults:\n"
        << "  --dec writes <input>.dec.json\n"
        << "  --enc writes <input>.enc.json\n"
        << "  multi-file mode always uses default output paths\n";
}

static std::vector<fs::path> parse_inputs(int argc, wchar_t **argv) {
    std::wstring s;
    for (int i = 2; i < argc; ++i) s += (s.empty() ? L"" : L" ") + std::wstring(argv[i]);
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
    return out;
}

static size_t workers_for(size_t files) {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    size_t by_mem = GlobalMemoryStatusEx(&m) ? static_cast<size_t>(std::max<unsigned long long>(1, m.ullAvailPhys / (128ull << 20))) : 1;
    size_t by_cpu = std::max(1u, std::thread::hardware_concurrency());
    return std::max<size_t>(1, std::min({files, by_cpu, by_mem, static_cast<size_t>(8)}));
}

static int run_one(const VeApi &ve, bool enc_mode, const Job &j, std::string &msg) {
    try {
        std::string in = read_all(j.in), out;
        if (enc_mode) {
            out = ve.encrypt(in);
            bool ok = false;
            std::string roundtrip = ve.decrypt(out, &ok);
            if (out.empty()) throw Error(10, "encrypt failed: output_len=0");
            if (!ok || roundtrip != in) {
                std::ostringstream e;
                e << "encrypt validation failed: ok_flag=" << (ok ? "true" : "false")
                  << ", encrypted_len=" << out.size() << ", roundtrip_len=" << roundtrip.size();
                throw Error(11, e.str());
            }
        } else {
            bool ok = false;
            out = ve.decrypt(in, &ok);
            if (!ok || out.empty()) {
                std::ostringstream e;
                e << "decrypt failed: ok_flag=" << (ok ? "true" : "false") << ", output_len=" << out.size();
                throw Error(10, e.str());
            }
        }
        write_all(j.out, out);
        std::ostringstream ok;
        ok << "ok input=" << narrow(j.in.wstring()) << " output=" << narrow(j.out.wstring())
           << " input_len=" << in.size() << " output_len=" << out.size();
        if (enc_mode) ok << " roundtrip_ok=true";
        msg = ok.str();
        return 0;
    } catch (const Error &e) {
        std::ostringstream fail;
        fail << "input=" << narrow(j.in.wstring()) << " output=" << narrow(j.out.wstring()) << " " << e.what();
        msg = fail.str();
        return e.code;
    } catch (const std::exception &e) {
        std::ostringstream fail;
        fail << "input=" << narrow(j.in.wstring()) << " output=" << narrow(j.out.wstring()) << " " << e.what();
        msg = fail.str();
        return 1;
    }
}

static int run_jobs(const VeApi &ve, bool enc_mode, const std::vector<Job> &jobs) {

    size_t n = workers_for(jobs.size()), ok = 0;
    std::atomic_size_t next{0};
    std::vector<Result> results(jobs.size());
    std::vector<std::thread> ts;
    ts.reserve(n);
    for (size_t t = 0; t < n; ++t) {
        ts.emplace_back([&] {
            for (;;) {
                size_t i = next.fetch_add(1);
                if (i >= jobs.size()) return;
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
    return ok == jobs.size() ? 0 : 20;
}

int wmain(int argc, wchar_t **argv) {
    try {
        if (argc < 3) return usage(), 64;
        std::wstring mode(argv[1]);
        bool dec = mode == L"--dec" || mode == L"-d", enc = mode == L"--enc" || mode == L"-e";
        if (!dec && !enc) return usage(), 64;

        bool comma = std::any_of(argv + 2, argv + argc, [](wchar_t *s) { return std::wstring(s).find(L',') != std::wstring::npos; });
        std::vector<fs::path> inputs = comma ? parse_inputs(argc, argv) : std::vector<fs::path>{unquote(argv[2])};
        if (inputs.empty()) return usage(), 64;
        if (!comma && argc > 4) return std::cerr << "too many arguments for single-file mode\n", 64;
        if (comma && inputs.size() <= 1 && argc > 3) return std::cerr << "comma-list mode does not accept a separate output path\n", 64;

        fs::path launch_cwd = fs::current_path();
        std::vector<Job> jobs;
        jobs.reserve(inputs.size());
        for (size_t i = 0; i < inputs.size(); ++i) {
            fs::path input = absolute_from(launch_cwd, inputs[i]);
            fs::path out = !comma && inputs.size() == 1 && argc >= 4
                               ? absolute_from(launch_cwd, fs::path(argv[3]))
                               : input.parent_path() / (input.filename().wstring() + (dec ? L".dec.json" : L".enc.json"));
            jobs.push_back({i, input, out});
        }

        VeApi ve = VeApi::load(install_dir_from_env());
        return run_jobs(ve, enc, jobs);
    } catch (const Error &e) {
        std::cerr << e.what() << "\n";
        std::cerr << "Create .env next to jy-draftc.exe with: JY_INSTALL_DIR=E:\\JianyingPro\\10.6.5.14040\n";
        return e.code;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        std::cerr << "Create .env next to jy-draftc.exe with: JY_INSTALL_DIR=E:\\JianyingPro\\10.6.5.14040\n";
        return 1;
    }
}
