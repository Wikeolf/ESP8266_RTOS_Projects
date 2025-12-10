FROM mcr.microsoft.com/devcontainers/base:ubuntu-22.04

# 避免在安装过程中出现交互式提示
ENV DEBIAN_FRONTEND=noninteractive

# 设置 SDK 和工具链的环境变量
ENV IDF_PATH=/opt/esp/ESP8266_RTOS_SDK
ENV PATH=$PATH:/opt/esp/xtensa-lx106-elf/bin

# 1. 安装系统依赖
RUN apt-get update && apt-get install -y \
    gcc git wget make libncurses-dev flex bison gperf \
    python3 python3-pip python3-venv \
    python-is-python3 \
    cmake ninja-build ccache \
    && apt-get clean -y && rm -rf /var/lib/apt/lists/*

# 2. 设置工作目录
WORKDIR /opt/esp

# 3. 下载并安装 Xtensa 工具链
RUN wget --no-verbose https://dl.espressif.com/dl/xtensa-lx106-elf-gcc8_4_0-esp-2020r3-linux-amd64.tar.gz \
    && tar -xzf xtensa-lx106-elf-gcc8_4_0-esp-2020r3-linux-amd64.tar.gz \
    && rm xtensa-lx106-elf-gcc8_4_0-esp-2020r3-linux-amd64.tar.gz

# 4. 克隆 ESP8266_RTOS_SDK (指定 v3.4 版本)
# === 修复开始 ===
# 修复 tinydtls 子模块链接失效的问题
# 将旧的 eclipse.org 链接替换为 github.com 的镜像链接
RUN git config --global url."https://github.com/eclipse/tinydtls.git".insteadOf "https://git.eclipse.org/r/tinydtls/org.eclipse.tinydtls" \
    && git clone --recursive --branch v3.4 --depth 1 https://github.com/espressif/ESP8266_RTOS_SDK.git $IDF_PATH
# === 修复结束 ===

# 5. 安装 Python 依赖
RUN python3 -m pip install --upgrade pip \
    && python3 -m pip install --no-cache-dir -r $IDF_PATH/requirements.txt

# 6. 设置串口权限
RUN usermod -aG dialout vscode

# 7. 权限修复：将 SDK 目录所有权移交给 vscode 用户
RUN chown -R vscode:vscode /opt/esp

# 恢复默认的前端交互设置
ENV DEBIAN_FRONTEND=dialog

# 设置默认用户
USER vscode
WORKDIR /workspaces