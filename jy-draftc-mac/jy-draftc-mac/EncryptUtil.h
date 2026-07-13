//
//  EncryptUtil.h
//  jy-draftc-mac
//
//  Created by laozhangdev on 2026/7/10.
//

#ifndef EncryptUtil_h
#define EncryptUtil_h

#include <string>

struct MsvcString {
    union {
        char small[16];
        char *ptr;
    } data{};
    unsigned long long size = 0;
    unsigned long long capacity = 15;
};

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

// dec(nullptr, &out, &in.s, &param.s, &good);


namespace lvve {
class EncryptUtils {
public:
    bool isEnable();
    void enable(bool);
    std::string encrypt(const std::string& inStr);
    std::string decrypt(const std::string& s1, const std::string& s2);
    std::string decrypt(const std::string& s1, const std::string& s2, bool& flag);
};
};

#endif /* EncryptUtil_h */
