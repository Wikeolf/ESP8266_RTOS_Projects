[中文版 (Chinese Version)](./README_cn.md)

# ESP8266 RTOS SDK Development Environment (Dev Container)

This is an ESP8266 development environment built on **VS Code Dev Containers**. It aims to provide a consistent, portable, and out-of-the-box development platform, eliminating the need for tedious toolchain and environment variable configuration on the local machine.

### ⚠️ Note
**This project has only been verified using VS Code on Arch Linux.**

## 🌟 Features

* **Base Image**: Ubuntu 22.04 LTS
* **SDK Version**: ESP8266_RTOS_SDK **v3.4** (Version locked for stability)
* **Toolchain**: xtensa-lx106-elf-gcc (8.4.0)
* **Build System**: Supports Make, CMake, Ninja.
* **Automated Fixes**: The Dockerfile includes an automatic fix for the broken `tinydtls` submodule link (replaced with a GitHub mirror).
* **IDE Integration**: Pre-configured VS Code C/C++ extension and IntelliSense paths.

## 📋 Prerequisites

Before starting, please ensure your host has the following software installed:

1.  **OCI Compatible Container Engine**: [Docker CE](https://docs.docker.com/engine/install/) is recommended (or [Podman](https://podman.io/)).
2.  **[Visual Studio Code](https://code.visualstudio.com/)**
3.  VS Code Extension: **[Dev Containers](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers)**

## 🚀 Quick Start (Arch Linux User Guide)

The following steps use **Arch Linux** as an example to detail how to configure the environment, user permissions, and USB rules to ensure the container can successfully access the host's ESP8266 hardware.

### 1. Add Udev Rules (Recommended)
To ensure the ESP8266 device obtains the correct permissions upon connection (avoiding "Permission denied" or device recognition issues), it is recommended to add udev rules.

Create and write the rule file (allowing ttyUSB devices to be read/written by the `uucp` group):
```bash
echo 'KERNEL=="ttyUSB*", MODE="0666", GROUP="uucp"' | sudo tee /etc/udev/rules.d/99-esp8266.rules
```

Reload udev rules to make them take effect immediately:
```bash
sudo udevadm control --reload-rules && sudo udevadm trigger
```

### 2. Start the Development Environment
After completing the above system configuration, initialize the project:

1.  **Clone the Repository**:
    ```bash
    git clone <YOUR_REPO_URL>
    cd <REPO_DIRECTORY>
    ```

2.  **Open the Project**:
    ```bash
    code .
    ```

3.  **Build and Enter Container**:
    * After VS Code starts, ensure the **Dev Containers** extension is installed.
    * Click **"Reopen in Container"** when the prompt appears in the bottom right corner.
    * Alternatively, press `F1` and type `Dev Containers: Reopen in Container`.
    * *The first build requires downloading the Docker image and toolchain, please wait patiently.*

4.  **Connection Verification**:
    * Connect the ESP8266 to the computer via USB.
    * After the container starts successfully, enter `ls -l /dev/ttyUSB0` in the VS Code integrated terminal.
    * If the file shows `rw-rw-rw` permissions, the mount is successful.

## 🛠️ Hardware Connection & Configuration

### Serial Device Mapping
According to the configuration in `devcontainer.json`, the container attempts to mount the host's `/dev/ttyUSB0` device by default:

```json
"runArgs": [
    "--privileged",
    "--device=/dev/ttyUSB0"
]
```

* **Linux Users**: Ensure your ESP8266 device is identified as `/dev/ttyUSB0` after connection. Also, check if the host user can normally read/write to the serial device. If it has a different name (e.g., `ttyUSB1`), please modify `devcontainer.json`.

* **Windows/Mac Users**: Docker Desktop does not directly support passing USB serial ports via `--device`.
    * **Windows**: You may need to use `usbipd`, or compile inside the container and use a flashing tool (such as Flash Download Tools) on the host.
    * **Mac**: It is also recommended to compile and generate the bin file inside the container, then flash it on the macOS side.

## 💻 Development Guide
Once the container starts successfully, you will enter a pre-configured terminal environment.
You can execute these commands in the root directory of a single project.

### Common Commands
Configure the project:
```Bash
make menuconfig
```

Compile the project:
```Bash
make
```

Flash firmware (requires serial permission):
```Bash
make flash
```

View serial logs:
```Bash
make monitor
```

Compile, flash, and open serial monitor:
```Bash
make flash monitor
```

### IntelliSense (Code Completion)
The project includes a `c_cpp_properties.json` file, with the following paths configured to support code navigation and completion:

* Current Workspace (`${workspaceFolder}/**`)
* SDK Components (`${env:IDF_PATH}/components/**`)
* Compiler path points to `/opt/esp/xtensa-lx106-elf/bin/xtensa-lx106-elf-gcc` inside the container.

## 📂 Directory Structure Explanation

* **Dockerfile**: Defines the environment build process, including installing Python dependencies, downloading the toolchain, and fixing the git submodule URL.
* **devcontainer.json**: Configures VS Code container behavior, including extension installation (`ms-vscode.cpptools`) and environment variables (`IDF_PATH`).
* **.vscode/c_cpp_properties.json**: C/C++ include paths specifically configured for the Linux environment.

## ⚠️ Notes

* **Permissions**: The default user inside the container is `vscode`, but it has sudo privileges. The ownership of the SDK directory `/opt/esp` has been transferred to the `vscode` user, so you can operate the SDK without using sudo. Since different distributions use different GIDs for serial management groups, the most universal method is to add udev rules. You can also add udev rules by filtering the device manufacturer.
* **Environment Variables**: `IDF_PATH` is automatically set in the `Dockerfile` and `devcontainer.json`, no manual configuration is required.