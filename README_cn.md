# ESP8266 RTOS SDK 开发环境 (Dev Container)

这是一个基于 **VS Code Dev Containers** 构建的 ESP8266 开发环境。它旨在提供一个一致、可移植且开箱即用的开发平台，无需在本地机器上繁琐地配置工具链和环境变量。

### ⚠️ 注意事项
**项目仅在Arch Linux环境下使用Vs Code验证过**

## 🌟 特性

* **基础镜像**: Ubuntu 22.04 LTS 
* **SDK 版本**: ESP8266_RTOS_SDK **v3.4** (已锁定版本以保证稳定性) 
* **工具链**: xtensa-lx106-elf-gcc (8.4.0) 
* **构建系统**: 支持 Make, CMake, Ninja。
* **自动化修复**: Dockerfile 中包含了针对 `tinydtls` 子模块链接失效的自动修复 (替换为 GitHub 镜像) [cite: 2]。
* **IDE 集成**: 预配置了 VS Code C/C++ 插件和 IntelliSense 路径。

## 📋 前置要求

在开始之前，请确保您的主机已安装以下软件：

1.  **OCI 兼容容器引擎**: 推荐使用 [Docker CE](https://docs.docker.com/engine/install/) (也可使用 [Podman](https://podman.io/))
2.  **[Visual Studio Code](https://code.visualstudio.com/)**
3.  VS Code 扩展: **[Dev Containers](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers)**

## 🚀 快速开始 (Arch Linux 用户向导)

以下步骤以 **Arch Linux** 为例，详细说明如何配置环境、用户权限及 USB 规则，以确保容器能顺利访问宿主机的 ESP8266 硬件。

### 1. 添加 Udev 规则 (推荐)
为了确保 ESP8266 设备插入后稳定获得正确的权限（避免 "Permission denied" 或设备无法识别），建议添加 udev 规则。

创建并写入规则文件 (允许 ttyUSB 设备被 uucp 组读写)：
```bash
echo 'KERNEL=="ttyUSB*", MODE="0666", GROUP="uucp"' | sudo tee /etc/udev/rules.d/99-esp8266.rules
```

重载 udev 规则使其立即生效：
```bash
sudo udevadm control --reload-rules && sudo udevadm trigger
```

### 2. 启动开发环境
完成上述系统配置后，进行项目初始化：

1.  **克隆仓库**:
    ```bash
    git clone <您的仓库地址>
    cd <仓库目录>
    ```

2.  **打开项目**:
    ```bash
    code .
    ```

3.  **构建并进入容器**:
    * VS Code 启动后，确保已安装 **Dev Containers** 扩展。
    * 右下角弹出提示时点击 **"Reopen in Container"**。
    * 或者按 `F1` 输入 `Dev Containers: Reopen in Container`。
    * *首次构建需要下载 Docker 镜像和工具链，请耐心等待。*

4.  **连接验证**:
    * 将 ESP8266 通过 USB 连接到电脑。
    * 容器启动成功后，在 VS Code 的内置终端输入 `ls -l /dev/ttyUSB0`。
    * 如果显示文件拥有 `rw-rw-rw` 权限，说明挂载成功。

## 🛠️ 硬件连接与配置

### 串口设备映射
根据 `devcontainer.json` 的配置，容器默认尝试挂载主机的 `/dev/ttyUSB0` 设备：

```json
"runArgs": [
    "--privileged",
    "--device=/dev/ttyUSB0"
]
```

* Linux 用户: 确保您的 ESP8266 设备连接后识别为 /dev/ttyUSB0。并检查宿主机用户是否能正常读写串口设备。如果是其他名称（如 ttyUSB1），请修改 devcontainer.json。

* Windows/Mac 用户: Docker Desktop 并不直接支持 --device 传递 USB 串口。

    * Windows: 您可能需要使用 usbipd 或仅在容器内编译，在宿主机使用烧录工具（如 Flash Download Tools）。

    * Mac: 同样建议在容器内编译生成 bin 文件，在 macOS 侧进行烧录。

## 💻 开发指南
一旦容器启动成功，您将进入一个预配置好的终端环境。
在单个项目的根目录下可以执行这些命令

### 常用命令
配置项目:
```Bash
make menuconfig
```

编译项目:
```Bash
make
```

烧录固件 (需串口权限):
```Bash
make flash
```

查看串口日志:
```Bash
make monitor
```

编译并烧录且打开串口:
```Bash
make flash monitor
```

### IntelliSense (代码补全)
项目包含 c_cpp_properties.json 文件，已配置好以下路径以支持代码跳转和补全：

* 当前工作区 (${workspaceFolder}/**)

* SDK 组件 (${env:IDF_PATH}/components/**)

* 编译器路径指向容器内的 /opt/esp/xtensa-lx106-elf/bin/xtensa-lx106-elf-gcc

## 📂 目录结构说明

* Dockerfile: 定义了环境构建过程，包括安装 Python 依赖、下载工具链以及修复 git submodule URL 。

* devcontainer.json: 配置 VS Code 容器行为，包括插件安装 (ms-vscode.cpptools) 和环境变量 (IDF_PATH)。

* .vscode/c_cpp_properties.json: 专门为 Linux 环境配置的 C/C++ 包含路径。

## ⚠️ 注意事项

* 权限: 容器内的默认用户为 vscode，但拥有 sudo 权限。SDK 目录 /opt/esp 的所有权已移交给 vscode 用户，无需使用 sudo 即可操作 SDK 。由于各发行版对串口管理用户组gid不同，最通用的方法就是添加udev规则，也可以通过筛选设备制造商来进行添加udev规则。

* 环境变量: IDF_PATH 已在 Dockerfile 和 devcontainer.json 中自动设置，无需手动配置。