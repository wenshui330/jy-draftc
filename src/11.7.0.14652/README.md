# 11.7.0.14652：可选 Bun 草稿加解密适配器

此目录提供 Windows x64 的单文件草稿加解密实现，使用本机剪映安装目录自带的 `vebun.exe` 加载同目录 `videoeditor.dll`。Python 封装只依赖标准库，无需另外安装 Node.js、Bun 或 Python 第三方库。

这是独立的 Python/JavaScript 入口，尚未接入根目录的 C++ 构建与分发脚本。现有 11.5/11.6 程序保持各自版本绑定。此适配器不提供草稿目录批处理、结构版本转换、首页登记或视频导出。

## 已验证的运行环境

| 项目 | 值 |
| --- | --- |
| 系统 / Python | Windows AMD64 / Python 3.11.9 |
| 剪映 | `11.7.0.14652` |
| 安装自带 Bun | `1.3.14` |
| `videoeditor.dll` | `73,839,384` 字节 |
| DLL SHA-256 | `737782e01257b875eedba5f67a8c955eb7ab32c5aa96b6f9bf8286903438c923` |

其他构建需要单独运行原生测试；不能由导出名相同推定兼容。桥接依赖 MSVC x64 的 32 字节 `std::string` ABI。

## 使用

需要 Python 3.9 或更高版本，以及本人已安装并有权使用的剪映。将下列占位路径替换为实际路径；`--jy-dir` 所指目录必须同时包含 `vebun.exe` 和 `videoeditor.dll`。

```powershell
cd src\11.7.0.14652

# 原生合成数据加密 -> 解密，检查字节完全相同
python jy_draftc.py -c --jy-dir "C:\path\to\JianyingPro\Apps\11.7.0.14652"

# 也可以通过进程环境变量选择安装目录（不自动读取 .env 文件）
$env:JY_INSTALL_DIR = "C:\path\to\JianyingPro\Apps\11.7.0.14652"

# 解密一个内容文件，显式写入另一文件
python jy_draftc.py -dc "D:\draft-copy\draft_content.json" -o "D:\work\draft.plain.json"

# 加密后，在子进程内强制验证解密结果与输入字节完全一致
python jy_draftc.py -ec "D:\work\draft.plain.json" -o "D:\work\draft.encrypted.json"

# 额外使用自己的密文文件做只读回环检查
python jy_draftc.py -c "D:\draft-copy\draft_content.json"
```

输出目录须已存在。默认拒绝覆盖已有输出；`-f` 只允许覆盖与输入不同的文件，始终拒绝原地覆盖。`--timeout` 控制每次子进程调用的超时，默认 45 秒。成功时退出码为 0；参数错误为 2，运行或加解密失败为 1。回执为 JSON，不输出草稿正文或密文。

使用前保留独立工程副本。命令仅转换指定内容文件，不会同步 `Timelines`、`draft_info` 等副本，也不会更新首页索引。完整工程写回仍需调用方处理对应版本的文档一致性；不要把单文件转换成功视为整个工程已正确保存。

## Python 接口

```python
from pathlib import Path
from bun_draft_crypto import BunDraftCryptoCodec

codec = BunDraftCryptoCodec(r"C:\path\to\JianyingPro\Apps\11.7.0.14652")
original = Path(r"D:\draft-copy\draft_content.json").read_bytes()
plain = codec.decrypt(original)  # UTF-8 JSON bytes
encrypted = codec.encrypt(plain)
assert codec.decrypt(encrypted) == plain
```

输入与输出保持字节形式；Python 只检查明文是 UTF-8 JSON 对象，不重新序列化。JS 桥接不解析草稿 JSON，因而不会将大于 `2^53` 的整数转换成 JS `Number`。每次调用使用独立临时目录及独立子进程，临时明文调用结束后删除；异常强制终止不构成安全擦除保证。

## 实现

1. 从显式安装目录启动原厂 `vebun.exe`，以该目录作为工作目录。
2. 使用 Bun FFI 和 `LoadLibraryW` 加载同目录 `videoeditor.dll`。
3. 用 `GetProcAddress` 查找 `EncryptUtils::decrypt`、`encrypt` 和 `enable` 的修饰名导出；不绑定 ordinal 或 RVA。
4. `encrypt` 后立即 `decrypt`，比较原始字节，空输出、原文透传、原生失败或回环不一致均报错。

剪映 11.7 的加解密依赖受支持的宿主上下文；直接从普通 Python/C++ 进程加载 DLL，可能收到成功标志但空输出。本实现复用安装自带的 Bun 宿主，不修改 DLL、不注入运行中的剪映进程、不改名程序，也不自行实现加密算法或提取密钥。

返回的原生 `std::string` 分配由一次性子进程退出时回收。此桥接不设计为常驻服务；若改为常驻，必须另行实现匹配的原生内存释放。

## 验证与复现

以下测试命令从仓库根目录运行。

无需剪映的 CLI 边界测试：

```powershell
python -m unittest discover -s src/11.7.0.14652/tests -p test_cli.py -v
```

在 Windows 上设置 `JY_INSTALL_DIR` 后运行完整测试：

```powershell
$env:JY_INSTALL_DIR = "C:\path\to\JianyingPro\Apps\11.7.0.14652"
python -m unittest discover -s src/11.7.0.14652/tests -v
```

原生测试真实调用本机引擎，覆盖中文、非 BMP 字符、`9007199254740993`、未知字段和空白保持、损坏密文拒绝，以及 CLI 转换和自检期间源文件内容与 mtime 不变。不满足 Windows / 安装环境时明确跳过，不以模拟加密替代。

2026-11-17 本次移植在 macOS 运行上述完整测试：13 项中 9 项通过、4 项 Windows 原生测试跳过。本次新增的字节接口和 CLI 成功路径尚未重新在 Windows 验证。

移植前同一具名导出桥接已在上述 Windows 环境验证：

- 78,844 字节的真实原生草稿解密、回加密、再次解密，内容相等；源文件摘要与 mtime 不变。
- 中文、非 BMP 字符、大整数和未知字段保持；损坏密文拒绝。
- 隔离真实工程只修改一条字幕后回读，其他解码字段与侧文件保持。

这些是原生 codec 和文件回读证据，不代表此适配器已完成 GUI 播放、保存重开、视频导出或全部版本兼容验收。真实草稿、原厂 DLL/运行时、素材、机器配置与凭据不随贡献提交。
