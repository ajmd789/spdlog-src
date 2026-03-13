# ZSend - 局域网文件传输工具

## 🚀 简介
ZSend 是一个基于 C++17 的轻量级跨平台局域网文件传输工具。它旨在解决局域网内设备间文件传输繁琐的问题，无需联网，无需登录，即开即用。

## 🏗️ 项目架构
项目采用模块化设计，主要包含以下核心组件：

*   **Discovery (发现模块)**: 基于 UDP 广播机制 (Port 8888)，自动发现局域网内的其他 ZSend 节点。
*   **Transfer (传输模块)**: 基于 TCP 协议 (Port 8888) 实现可靠的文件传输，包含握手、拒绝、传输、确认等状态流转。
*   **Config (配置模块)**: 管理用户配置（如昵称、下载目录），首次运行自动生成随机中文昵称（如“勤奋的蜜蜂”）。
*   **CLI (交互模块)**: 基于 `CLI11` 库，同时支持交互式菜单（TUI）和命令行参数调用（Scriptable）。

**技术栈**:
*   语言: C++17
*   构建系统: CMake 3.20+
*   核心库: `asio` (网络), `spdlog` (日志), `nlohmann/json` (序列化), `CLI11` (命令行), `doctest` (测试)

## 🛠️ 如何构建 (打包)

### 环境要求
*   CMake 3.20 或更高版本
*   支持 C++17 的编译器 (MinGW-w64, MSVC 2019+, Clang, GCC)

### Windows 构建方法
推荐使用项目自带的脚本进行构建，它会自动处理依赖下载和编译选项。

**方法 1: 使用脚本 (推荐)**
在项目根目录下打开 PowerShell 或 CMD，运行：
```powershell
.\scripts\build.bat
```
构建成功后，可执行文件位于 `build/zsend.exe`。

**方法 2: 手动构建**
```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```
*(注: 如果使用 Visual Studio，不需要指定 `-G "MinGW Makefiles"`)*

## 🏃 如何运行与使用

ZSend 支持两种运行模式：**交互模式** 和 **命令行模式**。

### 1. 交互模式 (Interactive Mode)
适合普通用户手动操作。直接双击 `zsend.exe` 或在终端运行不带参数的命令：
```powershell
.\build\zsend.exe
```
程序启动后会显示菜单：
*   **1. Send File**: 发送文件。输入文件路径后，会自动列出局域网内发现的设备，选择对应序号即可发送。如果没有发现设备，支持手动输入 IP。
*   **2. Receive File**: 接收模式。进入监听状态，当有文件请求时会提示确认。
*   **3. List Peers**: 查看当前在线的局域网小伙伴。

### 2. 命令行模式 (CLI Mode)
适合脚本集成或快速指令。

**发送文件**:
```powershell
# 语法: zsend send -f <文件路径> -t <目标IP>
.\build\zsend.exe send -f "C:\Users\Admin\Desktop\photo.jpg" -t 192.168.1.105
```

**接收文件**:
```powershell
# 语法: zsend recv [-d <保存目录>]
# 如果不指定目录，默认保存在当前目录或配置文件指定的目录
.\build\zsend.exe recv -d "D:\Downloads"
```

## ⚠️ 注意事项

1.  **防火墙拦截**: 
    *   首次运行时，Windows 防火墙可能会弹出拦截提示。**必须允许** ZSend 在“专用网络”和“公用网络”上进行通信，否则 UDP 发现功能将无法工作，导致找不到设备。
    *   如果发现不了设备，尝试手动关闭防火墙测试，或手动添加防火墙入站规则 (UDP/TCP 8888)。

2.  **端口占用**:
    *   ZSend 默认固定使用 **8888** 端口（UDP 和 TCP）。请确保该端口未被其他程序占用。

3.  **文件覆盖风险**:
    *   当前 MVP 版本在接收同名文件时，会直接覆盖旧文件（除非修改代码中的逻辑）。建议接收前确认目录下无同名重要文件。

4.  **路径格式**:
    *   在 Windows 终端输入文件路径时，建议使用引号包裹路径，特别是路径包含空格时。

## 🧪 开发与测试

*   **运行单元测试**:
    ```powershell
    .\build\zsend_tests.exe
    ```
*   **调试构建 (ASan)**:
    如果遇到崩溃或内存问题，可以使用 AddressSanitizer 构建调试版本：
    ```powershell
    .\scripts\buildAsan.bat
    ```
    构建产物位于 `build_asan` 目录。
