# jy-draftc

`jy-draftc` 是一个用于剪映 Windows 端草稿 JSON 的解密和回加密工具。
它能将草稿内的 `draft_content.json` 和 `draft_meta_info.json` 解密为明文json，并能在修改后回加密。

## 适用范围
1. 应用 `jianying_draft_encrypt_v2` 加密方案 的剪映版本

    明确验证 `10.3.0` 到 `10.9.0` (截至2026.7.15的最新版本)，其他未验证


2. 待更新...

## MacOS适配
感谢&nbsp;[@vhly](https://github.com/vhly)&nbsp;贡献的适用于MacOS的软件版本

[阅读文档](./jy-draftc-mac/README.md)

## 快速开始

### 准备
下载 Windows amd64 分发包后解压，目录里有：

```text
jy-draftc.exe
.env.example
SHA256SUMS.txt
```
打开PowerShell

```powershell
cd 解压路径
cp .env.example .env
```

编辑.env

```env
#这个目录下面必须能找到 videoeditor.dll
JY_INSTALL_DIR=你的剪映安装目录
```

### 开始使用

```powershell
cd 解压路径
```

解密单个文件：

```powershell
.\jy-draftc.exe -d "F:\Draft\draft_content.json"
```

回加密单个文件：

```powershell
.\jy-draftc.exe -e "F:\Draft\draft_content.json.dec.json"
```

手动指定输出路径：

```powershell
.\jy-draftc.exe -d "F:\Draft\draft_content.json" "F:\Draft\draft_content.plain.json"
.\jy-draftc.exe -e "F:\Draft\draft_content.plain.json" "F:\Draft\draft_content.json"
```

批量处理多个文件：

```powershell
.\jy-draftc.exe -d "F:\Draft\draft_content.json","F:\Draft\draft_meta_info.json"
.\jy-draftc.exe -e "F:\Draft\draft_content.json.dec.json","F:\Draft\draft_meta_info.json.dec.json"
```

多文件输入必须用英文逗号分隔。路径里有空格时，用英文双引号包住路径。


## 从源码构建

当前源码是单文件 C++ 程序，依赖 Windows API 和 C++17。

使用 MinGW-w64 构建：

```powershell
g++ -std=c++17 -O2 -municode -Wall -Wextra -static -static-libgcc -static-libstdc++ -o jy-draftc.exe src\jy-draftc.cpp
```

编译时可能出现 `GetProcAddress` 到函数指针的类型转换 warning，这是当前手动调用 C++ 导出函数的预期结果。

## 实现原理


```mermaid
flowchart TD
    A["jy-draftc.exe 启动"] --> B["读取同目录 .env"]
    B --> C["取得 JY_INSTALL_DIR"]
    C --> D["加载 videoeditor.dll"]
    D --> E["GetProcAddress 获取 EncryptUtils 导出函数"]
    E --> F["构造 MSVC std::string 兼容参数"]
    F --> G{"运行模式"}
    G -->|"解密"| H["调用 decrypt"]
    G -->|"回加密"| I["调用 enable true 和 encrypt"]
    I --> J["再次 decrypt 做回环校验"]
```


当前使用到的导出入口：

| 入口 | 作用 |
| --- | --- |
| `EncryptUtils::decrypt` | 把加密文本还原为明文 JSON |
| `EncryptUtils::enable` | 打开 DLL 内部加密开关 |
| `EncryptUtils::encrypt` | 把明文 JSON 加密回剪映格式 |

这里调用的是剪映 DLL 内部逻辑，所以工具本身不分发、不内置 `videoeditor.dll`。

### 已验证的解密入口

```text
?decrypt@EncryptUtils@lvve@@QEAA?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@AEBV34@0AEA_N@Z
```

近似对应：

```cpp
EncryptUtils::decrypt(
    std::string const& encryptedText,
    std::string const& paramJson,
    bool& ok
)
```

### 已验证的加密入口

```text
?enable@EncryptUtils@lvve@@QEAAX_N@Z
?encrypt@EncryptUtils@lvve@@QEAA?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@AEBV34@@Z
```


## 使用边界与免责声明

本项目仅用于学习、研究和技术验证。项目不研究、不提供、不支持任何形式的软件破解、商业限制绕过、版权保护绕过、账号/会员/云端鉴权绕过或未授权访问能力。

使用者应遵守所在地法律法规和相关软件用户协议，仅处理自己有权访问和修改的本地草稿文件。任何违法、侵权或未授权使用行为，均与项目作者无关。

本项目按 MIT License 以“原样”提供，不提供任何明示或暗示担保。使用本项目造成的任何数据损坏、软件异常、账号风险、法律纠纷或其他后果，均由使用者自行承担。


## 鸣谢
感谢[Linux.do](https://linux.do/)站点及其社区为项目开发和交流提供支持。

## 许可证

MIT License。详见 [LICENSE](LICENSE)。
