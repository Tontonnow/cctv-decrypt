# UDRM Remux

> 本工具仅用于个人学习研究，请遵守版权法规，严禁用于未经授权的媒体内容爬取、分发。

`udrm_remux` 是基于 FFmpeg `libav*` 开发的 UDRM 视频解密 + 封装工具，支持 Windows / macOS / Linux。

**主要功能：**

- 解密 UDRM 加密 HLS TS 流
- 重封装输出 MP4

---

## 目录

- [前置依赖](#前置依赖)
- [编译](#编译)
- [使用流程](#使用流程)
- [可选：环境变量注入密钥](#可选环境变量注入密钥)
- [命令行参数](#命令行参数)
- [常见问题](#常见问题)
- [免责声明](#免责声明)

---

## 前置依赖

### 公共依赖

1. clang 编译器
2. FFmpeg 开发库：`libavformat` / `libavcodec` / `libavutil`
3. `pkg-config`：编译时自动获取头文件与链接参数
4. `N_m3u8DL-RE`：下载加密 HLS m3u8 分片
5. Python3：运行 `license.py` 获取 CEK 密钥

### Linux（Ubuntu / Debian）

```bash
sudo apt update
sudo apt install -y clang pkg-config libavformat-dev libavcodec-dev libavutil-dev
```

### macOS（Homebrew）

```bash
brew install clang ffmpeg pkg-config
```

### Windows

推荐方案二选一：

#### 方案一：MSYS2（推荐）

```bash
pacman -S mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-ffmpeg mingw-w64-clang-x86_64-pkg-config make
```

#### 方案二：WSL2 Ubuntu

直接参考上方 Linux 安装步骤。

> **Windows 编译注意：**
>
> - 使用 MSYS2 的 clang，不要使用 MSVC。
> - `pkg-config` 需要能正常识别 FFmpeg 库。

---

## 编译

### 项目文件清单

- `udrm_remux.c`：源码
- `Makefile`
- `license.py`：密钥获取脚本

### 编译命令

进入项目目录后执行：

```bash
make
```

### 编译产物

- Linux / macOS：`udrm_remux`
- MSYS2 Windows：`udrm_remux.exe`

### 清理编译文件

```bash
make clean
```

---

## 使用流程

### Step 1：获取加密 m3u8 链接

1. 打开示例网页：

   <https://tv.cctv.com/2022/12/30/VIDEmXphIdgSNjC9IspKYIeG221230.shtml>

2. 打开开发者工具：
   - Windows / Linux：`F12`
   - macOS：`Command + Option + I`

3. 刷新页面，在网络面板搜索关键词：
   - `m3u8`
   - `hls_enc2_url`

4. 复制完整 m3u8 URL。

---

### Step 2：使用 N_m3u8DL-RE 下载加密 TS

> N_m3u8DL-RE 下载地址：
>
> <https://github.com/nilaoda/N_m3u8DL-RE/releases>

#### Linux / macOS

```bash
./N_m3u8DL-RE "https://xxx/index.m3u8" --save-name temp.mp4
```

#### Windows MSYS2

```bash
./N_m3u8DL-RE.exe "https://xxx/index.m3u8" --save-name temp.mp4
```

下载完成后得到加密文件：

```text
temp.mp4
```

---

### Step 3：初次运行获取 KID

#### Linux / macOS

```bash
./udrm_remux temp.mp4 output.mp4
```

#### Windows MSYS2

```bash
./udrm_remux.exe temp.mp4 output.mp4
```

正常报错提示如下，无需担心：

```text
CEI kid: zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
error: no CEK for kid 'zzz...' (add it to UDRM_KEYS or pass --key)
```

记录输出中的 `kid` 值。

---

### Step 4：使用 license.py 获取 CEK

```bash
python3 license.py "这里填入上一步拿到的kid"
```

脚本输出 CEK，即 16 字节十六进制密钥串。

---

### Step 5：携带 CEK 解密并重封装

#### Linux / macOS

```bash
./udrm_remux --key CEK_STRING temp.mp4 output.mp4
```

#### Windows MSYS2

```bash
./udrm_remux.exe --key CEK_STRING temp.mp4 output.mp4
```

执行完毕后，得到解密后的：

```text
output.mp4
```

---

## 可选：环境变量注入密钥

如果不想通过命令行传入密钥，可以使用环境变量 `UDRM_KEYS`。

### Linux / macOS

```bash
export UDRM_KEYS="KID:CEK"
./udrm_remux temp.mp4 output.mp4
```

---

## 命令行参数

```text
udrm_remux [OPTIONS] INPUT_FILE OUTPUT.mp4
```

| 参数 | 说明 |
| --- | --- |
| `--key <hex-cek>` | 指定解密 CEK |
| `-h, --help` | 打印帮助信息 |

---

## 常见问题

### 1. 编译报错：pkg-config cannot find libavformat

请根据系统检查依赖：

- **Linux**：确认已安装 `libavformat-dev`
- **macOS**：执行 `brew install ffmpeg`
- **Windows MSYS2**：必须使用 mingw64 环境

### 2. 提示 no CEK for kid

需要使用 `license.py` 获取对应 `kid` 的 CEK，然后通过 `--key` 参数传入。

### 3. N_m3u8DL-RE 下载失败

请检查以下内容：

- 网络是否正常
- UA 是否正确
- 防盗链 Cookie 是否缺失

部分链接下载时需要附带 Cookie 参数。

---

## 免责声明

> 本项目仅供计算机安全、多媒体编解码技术学习研究。
>
> 用户使用本工具所产生的一切法律责任由使用者自行承担。
>
> 严禁破解、复制、传播受版权保护的音视频内容。
