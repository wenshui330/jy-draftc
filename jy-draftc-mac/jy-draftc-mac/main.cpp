//
//  main.cpp
//  jy-draftc-mac
//
//  Created by laozhangdev on 2026/7/10.
//

#include <iostream>

#include "EncryptUtil.h"

using namespace std;
using namespace lvve;

int main(int argc, const char * argv[]) {
    // insert code here...
//    std::cout << "Hello, World!\n";
//    std::string inStr = "c8a7akD74A5d3utyiifbae4IIZ3E/a8jGHe47FqT60eHc5Ggbh8AQBTUh1i26I96B/2522jnSu8mA6001FlA5IzSiI7ebh+pUmtEd35tHkKxgaXxkniJyb4HQQVKC9cCbcbI7uqAZnuYVk6KjBbLs+uuepJYQ3eKM1SELjE3DoTOX0DH0JQS/FULjOwGYmRmmN+tytZEwjQJaMP6usCJompGMDkI1EaQmpa3+kLGgdxb8cGSjf2UABmmR4Bq7arYanWC0/787EJC3W2zXqa4VI89M5p//A8UHQDGCFDjCuYBHmplGwDJVFV6HrtBBwHsUaOm+CLKIQ+ksuWEQmuxxe7oaGb3X90=";
//    std::string pmStr = "{}";
//    bool result = false;
//    std::string outStr = lvve::EncryptUtils::decrypt(inStr, pmStr, result);
//    std::cout << "outStr: " << outStr <<std::endl;
    
    // 1. 设置加密
    EncryptUtils eu = EncryptUtils();
    eu.enable(true);
    const std::string origStr = "{\"readme\":\"This is only for study, this is a command line project in mac os. so you can modify and run for your logic, more information in README.md\",\"canvas_config\":{\"height\":1080,\"ratio\":\"16:9\",\"width\":1920},\"color_space\":0,\"create_time\":1659844631992,\"duration\":27466666,\"update_time\":1659844631992,\"version\":440000}";
    string encryptedStr = eu.encrypt(origStr);
    cout << "encryptedStr: " << encryptedStr << endl;
    
    string deStr1 = eu.decrypt(encryptedStr, "{}");
    cout << "deStr1: "<< deStr1 <<endl;
    
    return EXIT_SUCCESS;
}
