# LILYGO UI Launcher

[English](README.md) | 简体中文

LILYGO UI Launcher 是面向 LILYGO Linux 产品的 LVGL 应用启动器，CM0 是首个支持的
产品配置。设备端直接使用 Linux DRM/KMS 和 evdev，并保留 fbdev 兼容路径；主机端
使用 SDL2 窗口。应用是独立的可执行程序，Launcher 通过 JSON manifest 发现它们，
因此安装或卸载兼容的 `.deb` 后即可更新首页，无需重新构建 Launcher。

设备与模拟器默认使用 568x1232 的竖屏逻辑画布。首页使用 96 px 高的 AppKit 状态栏，
每页最多显示 18 个应用，竖屏为 3 列 6 行，横屏为 6 列 3 行。按空格键或从状态栏向下
滑动可打开快捷控制面板并切换方向，再按空格键或按 Esc 关闭面板；左右滑动或点击
分页指示器可切换应用页。主机模拟器在保留逻辑画布尺寸的同时，自动缩放窗口以适应
当前显示器；SDL 窗口使用高 DPI 渲染，缩放后仍保持清晰。

`--width` 和 `--height` 设置用于诊断的未旋转竖屏基准画布；
`--orientation portrait|landscape` 设置初始方向。SDL 窗口可自由调整大小，
便于验证较小或较宽的视口。

设备接入 TCA8418 键盘时自动切换横屏，启动时已连接也会生效；断开键盘时保留当前方向。
快捷控制面板始终可以自由切换横竖屏，键盘持续连接期间会保留手动选择，从应用返回后
也不会强制改回横屏。再次接入键盘会重新触发横屏。其他键盘和 SDL 模拟器的主机键盘
不会触发自动旋转。
设备登录后，快捷控制面板还提供 **Lock screen（锁屏）**，保留当前账户会话，
输入该账户的密码即可解锁；**Sign out（退出登录）** 则会结束会话。快捷控制面板还提供参考
iOS 风格的 **Power off（关机）**、**Restart（重启）**、**Brightness（亮度）** 和
**Volume（音量）** 卡片。亮度和音量支持竖向拖动，也可通过键盘以 5% 步进调整。
关机和重启始终需要确认；模拟器只显示安全完成提示，不会关闭或重启主机。
连接检测通过 `/dev/i2c-*` 读取 TCA8418 的配置寄存器。拔插使芯片配置复位、但输入节点
仍保留时，Launcher 会重新绑定该键盘的内核驱动，恢复矩阵扫描和中断，再发现新的输入
设备。恢复失败会重试，不会覆盖手动选择的屏幕方向。这需要访问键盘所在的 I²C 适配器
和驱动的 sysfs bind/unbind 控制节点；安装的 root 服务具备这些权限。
前台应用运行期间，Launcher 也会继续检测并恢复键盘；AppKit 负责在旧输入设备被删除后
重新打开新的 evdev 节点。由于 AppKit 静态链接进应用，已有应用需要使用包含重连修复的
SDK 重新构建，才能在不重启应用的情况下恢复输入。

## 项目结构

```text
src/
  *.cpp, *.h  Launcher 运行逻辑与 UI 源码
tests/        测试与 UI 渲染测试源码
docs/         设计文档及历史验证、部署记录
assets/
  images/     图片资源
  audio/      音频资源
  fonts/      字体资源
  app-icon.png  128x128 发布图标
  previews/     发布预览图（不安装到设备）
```

Launcher 源码直接放在 `src/`，测试放在 `tests/`。应用各自维护独立仓库，
Launcher 仓库不存放、编译、测试或打包应用源码。

[设计文档索引](docs/README.md)包含生命周期、图形渲染、SDK 打包和 UI 设计要求。
按日期保存的优化与部署记录位于[历史归档](docs/archive/README.md)。

LILYGO UI AppKit 在独立仓库维护，本项目通过 `third_party/cm0-appkit` Git 子模块
固定其版本。Launcher 静态编译 AppKit 及其内置的 LVGL。AppKit 的 C API 支持
C 和 C++ 应用，消费者通过源码 SDK 的 CMake 包调用
`find_package(LilyGoUI CONFIG REQUIRED)`。

设备上的 `lilygo-ui-appkit-dev` 0.1.0 或更新版本提供源码 SDK、完整的 Inter、
Source Han Sans CN（思源黑体）和 Font Awesome 运行时字体及其 OFL 许可证。
应用共享这些字体文件；AppKit 和 LVGL 代码则分别编入各应用的可执行文件。
更新 AppKit 代码需要更新固定的子模块版本，并重新构建应用。

克隆时包含子模块，或在已有 checkout 中初始化子模块：

```sh
git clone --recurse-submodules https://github.com/LILYGO-UI/launcher.git
git submodule update --init --recursive
```

## 使用 LPM 管理项目

本仓库使用 [LPM](https://github.com/LILYGO-UI/lpm) 作为主要开发入口。
使用 `uv`（推荐）或 `pip` 安装一次即可：

```sh
uv tool install "git+https://github.com/LILYGO-UI/lpm.git"
# 或：python3 -m pip install "git+https://github.com/LILYGO-UI/lpm.git"
lpm --version
```

`lpm.toml` 是 Launcher 包名、版本、应用 ID、发布元数据、构建预设和部署默认值的
唯一人工维护来源。CMake 在配置时通过 LPM 校验该文件，并从中生成项目与 Debian
软件包元数据。

常用工作流程：

```sh
lpm start                 # 构建并在后台启动主机模拟器
lpm stop                  # 停止后台模拟器
lpm test                  # 构建并运行主机测试，写入 .lpm/test-results.xml
lpm pack                  # 交叉构建并将 Debian 包写入 dist/
lpm deploy                # 将最新软件包部署到配置的目标设备
lpm metadata --format publish-json --output .lpm/publish.json
```

需要在终端直接查看模拟器日志时，使用 `lpm start --foreground`。SSH 目标的默认值
位于 `lpm.toml` 的 `[deploy]` 中，可通过 `--host`、`--user`、`--port` 和
`--target` 等命令行选项覆盖。执行 `lpm login` 后，可使用 `lpm publish` 将通过
校验的软件包及其元数据提交审核。

## 运行主机模拟器

安装 CMake、pkg-config、SDL2、FreeType 和 LPM。在 macOS 上：

```sh
brew install cmake pkg-config sdl2 freetype glib openssl@3
git submodule update --init --recursive
lpm start --foreground
lpm test
```

主机预设使用本仓库的 AppKit 源码子模块，并静态构建其中的 LVGL 9.5.0，配置时
无需下载 LVGL。主机字体从子模块资源目录加载，无需安装 AppKit 二进制 SDK 或设置
`CMAKE_PREFIX_PATH`。若复用之前采用二进制 SDK 的构建目录，需先重置一次 CMake
缓存：`cmake --fresh --preset host-simulator`（要求 CMake 3.24 或更新版本）。

Launcher 与各独立应用均使用 `LV_DEF_REFR_PERIOD=20` 编译 LVGL，对应 CM0 屏幕的
50 Hz 模式。显示、动画和输入轮询的默认周期统一为 20 ms，固定旧版 AppKit 的项目
也会使用该配置。由于静态链接，各应用都需要重新构建并部署后生效；实际帧率仍取决于
渲染耗时，视频播放和模拟器保留各自的内容时钟。

主机模拟器使用虚拟凭据登录：用户名 `pi`，密码 `raspberry`。这些凭据支持登录、
锁屏解锁和退出登录，无需创建或认证开发机的系统账户；启动的应用继续以当前开发机
用户运行。虚拟认证只编译进主机模拟器，设备构建使用真实 Linux 账户和 PAM。

主机模拟器不内置应用。安装应用包，或将 `LAUNCHER_APP_DIRS` 指向应用 manifest
所在目录，即可在首页显示应用。每个应用均从自己的仓库独立构建和打包。

Launcher 和应用日志写入 stderr，设备端由 systemd-journald 收集。
应用不链接 Launcher 的日志库。

平台集成和测试时，可通过 `CM0_POWER_SUPPLY_DIR`、`CM0_INPUT_DIR` 和
`CM0_NETWORK_DIR` 覆盖状态栏读取硬件信息的根目录。

## Launcher 界面

`src/launcher_view.cpp` 直接通过 LVGL C API 构建 View。`src/ui.cpp` 是 C++
Controller，负责从运行时注册表填充应用、连接交互行为，并通过具有明确类型的 View
引用应用方向变化。设备程序、渲染测试和主机模拟器编译同一套源码，不包含 XML 或
UI 代码生成步骤。

公共状态栏和 home indicator 由独立的 AppKit 统一实现。Launcher 将它们放入
自己的 View；独立应用则由 AppKit 在 `lv_layer_top()` 上提供相同的系统界面元素。

[应用生命周期与显示交接设计](docs/launcher-app-lifecycle.md)说明应用发现、
进程启动、显示交接、退出恢复和运行时资源的完整流程。

[UI 设计规范](docs/ui-design-guidelines.md)规定响应式布局与排版要求，包括视口
适配、方向变化、Inter 与 Source Han Sans CN 的使用、字号、中西文混排、运行时
字体加载、增量字体及字体授权。

## 生产环境启动

目标镜像不运行桌面会话或合成器。Launcher 作为系统主界面，在 `multi-user.target`
下通过 `lilygo-ui-launcher.service` 启动：

```sh
/usr/bin/lilygo-ui-launcher --display drm --drm /dev/dri/card0 \
  --input /dev/input/by-path/platform-3f205000.i2c-event
```

该服务排在 `network-online.target` 和 `time-sync.target` 之后启动，因此状态栏首次
读取时间时，系统校时服务已经有机会设置 `CLOCK_REALTIME`。镜像仍需启用实际的校时
服务（例如 `systemd-timesyncd` 或 `chrony`），并通过 `/etc/localtime` 配置设备时区；
Launcher 直接使用系统时钟和时区，不单独实现 NTP 客户端。

服务直接打开 DRM/KMS 和 evdev。启动前，软件包安装脚本会禁用已配置的显示管理器，
屏蔽 `getty@tty1.service`，并将默认 target 切换为 `multi-user.target`。原来的
target 和显示管理器状态记录在 `/var/lib/lilygo-ui-launcher`，供卸载时恢复。
软件包不安装 XDG autostart 或应用菜单入口。

Launcher 以 root 运行，用于管理显示设备和设置首次账号。pi-gen 启用首次启动向导时，
`lilygo-ui-launcher-first-boot prepare` 检测 `rename-user` 留下的 SSH 重命名提示配置、
`piwiz.desktop` 或已启用的 `userconfig.service`，将原账号的 UID 和名称保存到仅 root
可写的 `/var/lib/lilygo-ui-launcher/first-boot-pending`。随后进入完整设置流程：
地区与语言 → 账号 → Wi-Fi → 更新 → 完成。账号页包含用户名、密码、确认密码；
用户名预填为原账号名称，可以保留 `pi`，也可以修改。

设置会原地重命名原账号、主目录及同名主用户组，保留 UID、已有附加组和 SSH 公钥等
预置文件，将镜像的禁用登录 shell 改为 `/bin/bash`，并同步 `subuid`、`subgid` 及已有
`010_pi-nopasswd` 中的账号名。账号会加入
`sudo` 组，沿用系统 sudo 策略。软件包依赖 `sudo`，缺少该组时会在修改账号前报错；
Launcher 不添加免密码规则。如果系统完全没有 UID 1000 账号，则使用 `useradd` 创建
首个账号，并加入 `sudo` 及可用的 `video`、`render`、`input` 组。

密码通过 `chpasswd` 的标准输入设置，并经 PAM 验证。成功后才写入
`/var/lib/lilygo-ui-launcher/active-user`，继续后续设置，完成向导时才移除上游向导入口、临时向导账号及其
sudo 规则、SSH 重命名提示和 autologin 标记，禁用 `userconfig.service`。收尾不会
改变 Launcher 的显示服务或启动目标。设置失败可依据原 UID 重试；仅 root 可读写的
`first-boot-progress` 保存已完成步骤。中途重启后须先用已保存的账号认证，再继续设置；
收尾中断可以重试。重命名开始后，重试使用已选定的用户名直至完成设置。密码不会写入
Launcher 状态文件或命令行。

如果镜像设置了 `DISABLE_FIRST_BOOT_USER_RENAME=1` 并预配账号，Launcher 直接显示
普通登录页，首次认证成功后记录账号，不会重命名或修改其用户组。之后每次启动显示
登录页；应用以认证账号的 UID、GID、附加组运行，使用其 HOME 和 XDG 目录。
`userconfig.service` 的 systemd drop-in 防止控制台向导与 Launcher 争用 tty1。

登录后，打开 **Quick controls → File sharing**，即可接收同一局域网内浏览器发送的文件。
浏览器选择设备名 `lilygo_xxxx`（蓝牙 MAC 的最后两个字节）后会自动发现 GATT 服务并建立局域网直连，无需输入配对码。
蓝牙仅负责发现设备和快速协商；DataChannel 建立后浏览器自动释放蓝牙连接，文件通过同一局域网传输。
每次发送文件前，设备会显示文件名和大小，必须在设备上点按 **Receive file** 才会开始传输；
选择 **Decline** 会拒绝该文件。每个文件分别确认，无需额外部署 WSS 信令或中转服务。
关闭共享面板、锁屏或退出登录都会停止共享。

文件保存到认证用户私有的 `$HOME/Downloads`，先写入临时文件，校验大小和 SHA-256 后
原子提交；重名文件自动添加序号。Launcher 以 root 运行时不会把自身继承的 `HOME` 当作
目标路径。浏览器默认单文件上限为 512 MiB，详见[局域网文件共享说明](docs/file-sharing.md)
中的连接步骤、协议、依赖和测试方法。

旧版向导已创建的账号需由现有管理员执行一次 `sudo usermod -aG sudo wing`（root shell
中直接执行 `usermod -aG sudo wing`），然后退出并重新登录，包括重新连接 SSH。
仅升级 Launcher 不会更改已有账号的权限。

已有账号的目标板需要重复测试向导时，保留原管理员，使用独立临时来源账号。
停止 Launcher 后备份并移走四项向导状态，创建仅 root 可写的
`first-boot-pending`（内容为临时账号的 `UID:用户名`），再启动 Launcher。
`prepare` 不会强制重置已完成的设置；仅删除 `active-user` 也不会重新开启向导。
[板端重进与恢复步骤](tests/README.md#re-entering-setup-on-an-already-configured-board)
包含完整命令及临时账号清理方法。只预览页面可在模拟器使用 `--setup-demo`。

构建需要本地向导的 pi-gen 镜像时，不设置 `FIRST_USER_PASS`，且不设置
`DISABLE_FIRST_BOOT_USER_RENAME`（或设为 `0`），即可不携带默认密码并由 Launcher
完成账号设置。该实现参考了官方
[rename-user](https://github.com/RPi-Distro/userconf-pi/blob/pios/trixie/rename-user)、
[userconf](https://github.com/RPi-Distro/userconf-pi/blob/pios/trixie/userconf) 和
[cancel-rename](https://github.com/RPi-Distro/userconf-pi/blob/pios/trixie/cancel-rename)
的账号生命周期，不依赖桌面环境。

地区页从系统目录读取国家、UTF-8 语言、时区和 XKB 键盘布局，通过
[raspi-config](https://github.com/RPi-Distro/raspi-config/blob/trixie/raspi-config)
配置 Wi-Fi 国家、系统语言、时区和系统键盘，并验证保存结果后继续。语言选项设置系统
locale，当前 Launcher 界面仍为英文。键盘选项设置 Linux 控制台和桌面布局；Launcher
软键盘及 AppKit 的原始 evdev 文本映射目前仍使用 US 布局。

Wi-Fi 页使用 NetworkManager，可扫描网络、手动输入隐藏 SSID、连接 WPA2/WPA3、
Enhanced Open 或开放网络，也可以跳过。密码通过标准输入传递，不出现在命令行或
Launcher 日志；系统连接配置由 NetworkManager 保存。企业 802.1X 和 WEP 网络需由
管理员另行配置。

更新页可以检查、安装、失败后重试或返回 Wi-Fi，也可以跳过。
`lilygo-ui-launcher-first-boot-update.service` 独立运行 apt，升级 Launcher 软件包时仍可
继续安装。更新会刷新索引、升级软件包及新增依赖、保留现有配置，进度保存在
`first-boot-update`。检查或安装期间禁止切换页面；中断的安装可重试。
完成向导后进入主页；如果更新提示需要重启，请在设置结束后重启设备。

可在模拟器中预览完整流程，所有操作仅保存在内存中，不修改宿主系统：

```sh
build/host-simulator/lilygo-ui-launcher --setup-demo
```

浏览器选择及启动分区 `userconf` / `userconf.txt` 导入不在当前流程范围内；
无屏幕镜像请在构建时预配账号。

除触摸输入外，Launcher 还会根据 evdev 能力发现 GPIO `KEY_POWER` 事件设备。
收到按压事件序列时，界面显示包含“正在关机”和“请继续按住电源键”的遮罩；GEK100
脉冲流停止 300 ms 后恢复正常页面。板端长按辅助程序负责判定持续三秒后的关机操作。
诊断时可用 `--power-input PATH` 指定事件节点；正常部署可不设置，使 Launcher
不依赖具体的事件编号或 udev 路径名称。

`lilygo-ui-launcher-console` 在服务占用 KMS 时临时解绑 framebuffer console
驱动，并在服务停止后重新绑定。Launcher 与应用按顺序使用同一个 DRM 设备。
当 AppKit SDK 支持 DRM/EGL 交接时，Launcher 会暂停 timer、完成待处理的 page
flip、释放 DRM master 并关闭 evdev 输入，然后启动应用；同时保留 UI、EGL
上下文、纹理缓存和最后呈现的 framebuffer。应用退出后，Launcher 重新取得 master、
呈现保留帧、重开 evdev 并重新绑定手势，随后恢复更新。软件 DRM、旧 SDK 或交接
失败时，采用销毁并重建显示与 UI 的路径。

保留 Launcher 缓存会增加应用运行期间的内存占用，应在目标设备上测量两者同时
运行时的负载。设置 `LILYGO_UI_DRM_RECREATE=1` 可强制使用完整重建路径，用于
诊断或对比。`switch prepared` 和 `switch restored` 日志记录 `release_ms`、
`restore_ms` 以及是否保留显示。这些指标衡量进程侧切换耗时，不是应用首帧或物理
显示扫描输出的时间。所有权、回退和诊断细节见
[应用生命周期设计](docs/launcher-app-lifecycle.md)。应用每次仍以新的独立进程启动。

## 为 CM0 交叉编译

`cm0-cross` 在首次配置时，从固定的
[`0.1.0` GitHub release](https://github.com/LILYGO-UI/CM0BspBuilder/releases/download/0.1.0/cm0_sdk.tar.gz)
下载 CM0 BSP SDK。CMake 校验归档的 SHA-256 后，将其解压到
`.cache/cm0-bsp/0.1.0`；后续构建复用该缓存，无需本地 CM0BspBuilder 仓库。

BSP 提供编译器和基础系统库。AppKit 从固定子模块编译，sysroot 无需安装 AppKit
二进制包。使用自定义 BSP 时，通过 `-DCMAKE_TOOLCHAIN_FILE=...` 传入其工具链。
原生 C++ 文件分享辅助程序还需要 GLib/GIO 和 OpenSSL 开发文件。使用固定 BSP
时，CMake 自动下载并校验固定 SHA-256 的 ARM64 开发包，缓存于
`.cache/cm0-bsp/transport/trixie-arm64-v1`，同时补齐 GLib 所需的 sysprof、zlib
开发元数据。该独立缓存复用 BSP 运行库，不修改原始 sysroot。自定义 BSP 应提供
与其运行库匹配的开发文件。
WebRTC 依赖从固定版本源码编译，详见[文件分享说明](docs/file-sharing.md#device-architecture-and-installed-files)。

设备后端不依赖 SDL、Wayland、xkbcommon、spdlog 或 fmt，默认使用软件渲染。
若 BSP 包含 `libgbm-dev`、`libegl-dev` 和 `libgles-dev`，可在独立构建目录中设置
`-DLILYGO_UI_RENDERER=opengles`，使用 AppKit 的 GLES2 纹理缓存和 GPU 显示旋转。
固定的 BSP 0.1.0 需要先在单独的 sysroot 中补齐这些开发包，再直接传入该 BSP
的工具链。

配置方法及软件/GPU 对比工具见
[AppKit 设备图形说明](third_party/cm0-appkit/README.md#device-graphics)。GPU
应用包会增加 `libgbm1`、`libegl1` 和 `libgles2` 依赖。EGL 初始化失败会在启动时
报错；没有可用 EGL/GBM 驱动的设备应使用软件渲染构建。

实验选项 `-DLILYGO_UI_RENDERER=nanovg` 通过 NanoVG GLES2 在 GPU 上绘制图元，
要求 EGL 支持 stencil-8/MSAA-4，并使用独立构建目录。SDK 图形对比工具在静态内容
滚动和持续变化的控件场景中，将 NanoVG 与软件绘制、GLES 缓存渲染进行对比。生产环境的
渲染器选择应以目标设备实测为依据，不能仅凭 GPU 构建成功就切换方案。

[图形渲染设计](docs/graphics-rendering.md)说明渲染器选择、旋转与缓冲所有权、
私有 VC4 运行库，以及相关 CPU/内存取舍。NanoVG 的阴影与圆弧效果仍与参考画面
存在差异，因此保留为实验选项。
[应用生命周期设计](docs/launcher-app-lifecycle.md)定义显示保留、失败恢复和切换
耗时的测量边界。

[DRM 硬件测试指南](tests/README.md)包含临时子进程退出测试、真实应用首帧与
返回计时、手势退出和完整重建对比。测试需要独占 CM0 目标设备的 DRM，指南也提供
生产服务使用的私有 VC4 运行库环境配置。[历史报告](docs/archive/README.md)保留
此前的硬件结果、部署版本和恢复依据，不代表后续代码已重新验证。生产后端使用
DRM，fbdev 保留为兼容路径。

```sh
lpm pack
file build/cm0-cross/lilygo-ui-launcher
DESTDIR="$PWD/build/cm0-root" cmake --install build/cm0-cross
```

直接访问显示设备时，使用
`lilygo-ui-launcher --display drm --drm /dev/dri/card0 --drm-connector ID --input
/dev/input/by-path/platform-3f205000.i2c-event`。稳定的 by-path 符号链接可避免依赖
启动时分配的事件编号。未使用 KMS 的镜像仍可采用 `--display fbdev --fbdev /dev/fb0`。
添加 `--orientation landscape` 可从横屏启动；之后通过快捷控制面板修改的方向在
当前 Launcher 会话内保持。

## 应用接入约定

LILYGO UI 应用使用 `lilygo-ui` 包名前缀和 `cc.lilygo.ui` 应用命名空间。例如，
组件名为 `file-manager` 时，Debian 包和可执行文件名为 `lilygo-ui-file-manager`，
应用 ID 为 `cc.lilygo.ui.FileManager`。manifest 与可执行文件的 descriptor 使用
相同的应用 ID。

应用包安装可执行文件、发现用 manifest 和 Launcher 图标：

```text
/usr/bin/lilygo-ui-hello
/usr/share/launcher/apps/cc.lilygo.ui.Hello.json
/usr/share/icons/hicolor/128x128/apps/cc.lilygo.ui.Hello.png
```

manifest 格式如下：

```json
{
  "schema_version": 2,
  "id": "cc.lilygo.ui.Example",
  "name": "Example",
  "description": "Short home-screen text",
  "icon": "../../icons/hicolor/128x128/apps/cc.lilygo.ui.Example.png",
  "order": 100,
  "executable": "../../../bin/lilygo-ui-example"
}
```

`id` 必须全局唯一，并与可执行文件的 descriptor 一致。`icon` 可以是绝对路径，
也可以是相对 manifest 的路径。应用包在 hicolor 图标主题中安装以应用 ID 命名的
128x128 PNG，Launcher 将其缩放到首页磁贴中。如果未提供图标、文件缺失或无法
识别为 PNG，则使用内置默认图标。已有软件包使用的旧符号图标名称仍受支持。

manifest 按优先级从 `$LAUNCHER_APP_DIRS`、用户 XDG 数据目录、
`/usr/local/share/launcher/apps` 和 `/usr/share/launcher/apps` 读取。
无效 manifest、缺失的可执行文件和重复 ID 会被跳过。

应用子进程会在 `initgroups`、`setgid` 和 `setuid` 后以已认证的 Linux 用户运行，
Launcher 只清理 root 专用的环境变量，不会强制设置 `PR_SET_NO_NEW_PRIVS`。因此 Settings
在修改 SSH 或 Serial Console 配置时，可以按系统已有的 `sudo` 或 polkit 策略请求权限；
仍须通过该用户正常的授权和认证，Launcher 不提供 root 凭据。

首页显示期间每两秒重新扫描一次目录。新安装的应用无需重启 Launcher 即可出现，
已卸载的应用则从首页移除。每个应用在独立子进程中运行，拥有自己的 LVGL 状态。
子进程占用显示设备时，Launcher 暂停自己的 LVGL timer；子进程退出后重绘首页。

应用提供 `cm0_app_get_descriptor`，在 `open` 中以传入的 `context->root` 为父对象
创建 LVGL View，并在 `close` 中释放非 LVGL 资源。由于系统界面元素位于顶层，
应用也可加载自己的 LVGL screen。AppKit 生成的 `main` 在应用进程内负责
`lv_init()`、显示与输入初始化、事件循环和 `lv_deinit()`，同时提供公共状态栏与
home indicator。从底边上滑或调用 `context->request_exit()` 会退出进程。
退出后恢复首页是 Launcher 的策略，不是应用 API 的职责。

Launcher 和应用都可以调用 `lv_init()`，因为两者属于不同进程，拥有独立地址空间
和 LVGL 全局状态。它们不能同时驱动同一个 framebuffer。Launcher 在子进程执行
`exec` 前停止自己的 LVGL timer 和显示访问，等待应用退出后再恢复显示并重绘首页。

每个应用从自己的独立仓库根目录构建和打包。其 `cm0-cross` 预设应使用相同的固定
远程 BSP 下载流程，不依赖其他 checkout 的路径；AppKit 依赖应固定为 Git 子模块：

```sh
git submodule update --init --recursive
cmake --preset cm0-cross
cmake --build --preset cm0-cross --parallel
ctest --test-dir build/cm0-cross --output-on-failure
cpack --config build/cm0-cross/CPackConfig.cmake -G DEB
```

生成的软件包只包含该应用及其可选的 Launcher 发现 manifest，不依赖
`lilygo-ui-launcher` 软件包。

## 构建 Launcher Debian 包

```sh
lpm pack
```

生成的 `.deb` 写入仓库的 `dist/` 目录。Launcher CPack 只生成
`lilygo-ui-launcher` 包，包含 Launcher 可执行文件和 systemd unit。每个应用仓库
独立生成自己的 `lilygo-ui-*` 包；应用包不属于 Launcher 构建的组件，也不依赖
`lilygo-ui-launcher`。

安装 Launcher 包会启用 `lilygo-ui-launcher.service`，选择 `multi-user.target`，
并禁用图形显示管理器。软件包不包含桌面会话入口，运行时依赖 `libc6`、
`libstdc++6`、`libdrm2`、`libfreetype6`、AppKit 字体栈使用的图像与压缩库，以及
`lilygo-ui-appkit-dev`（至少 0.1.0，由 `project.compatibility.min_appkit_version`
记录）。dev 包提供源码 SDK、共享字体文件和许可证，图形依赖随所选渲染器变化。
AppKit 和 LVGL 本身静态链接，因此设备不需要 AppKit 共享库或 ABI capability 包。
Launcher 向应用传递 DRM、connector、evdev 和画布参数，使应用运行时能够接管
同一个显示设备。

[SDK 与打包设计](docs/sdk-packaging.md)说明源码与运行时资源归属、软件包校验和
部署验证要求。

Launcher 包以当前版本提供旧 `cm0-launcher` 包的兼容身份，并在迁移时声明与旧包
冲突和替换关系。

### 通过 Store 更新 Launcher

Launcher 更新不能在 `lilygo-ui-launcher.service` 内执行 `dpkg`。Store 等由
Launcher 启动的应用都位于该服务的 cgroup 中，而旧包的 `prerm` 会在升级时停止该服务。
因此软件包提供仅允许 root 调用的交接辅助程序和独立的 oneshot 服务：

```text
/usr/lib/lilygo-ui-launcher/lilygo-ui-launcher-update
/usr/lib/systemd/system/lilygo-ui-launcher-update.service
```

Store 下载并验证 Registry 中已发布的版本后，将软件包交给辅助程序，然后退出
前台应用：

```sh
pkexec /usr/lib/lilygo-ui-launcher/lilygo-ui-launcher-update \
  queue /tmp/downloaded-launcher.deb VERSION SHA256
```

`queue` 在返回前将软件包复制到 root 拥有的存储目录，并异步启动 oneshot 服务。
如果调用者是 Store 的特权安装进程，worker 会先等待安装器和 Store UI 退出，避免旧
Launcher 服务停止时连带终止 Store；等待超时则放弃本次安装。该服务在 Launcher 的
cgroup 之外运行，再次校验 SHA-256 及 Debian 的 `Package`、
`Version`、`Architecture` 字段，拒绝同版本重装或降级，执行 `dpkg` 并验证已安装
版本，最后才重启 Launcher。可通过
`/usr/lib/lilygo-ui-launcher/lilygo-ui-launcher-update status` 或
`/var/lib/lilygo-ui-launcher/last-update` 查看最近状态；完整日志写入 journald 的
`lilygo-ui-launcher-update.service` 单元。

Store 必须将 `lilygo-ui-launcher` 视为不可卸载的系统包，只提供更新操作，并使用
交接辅助程序，不能沿用普通的同步 `dpkg --install` 路径。Debian 包同时标记为
`Priority: required`，其 `prerm` 默认拒绝卸载，除非管理员明确设置
`LILYGO_UI_LAUNCHER_ALLOW_REMOVE=1`。这套机制在包含辅助程序的基线包安装后生效；
仍运行旧 Launcher 的镜像需要先通过系统镜像或管理员部署一次基线包。

使用 `lpm.toml` 中配置的目标设备部署最新生成的软件包：

```sh
lpm deploy
```

冷启动时，即使设备节点和 connector 已就绪，其他启动阶段的客户端仍可能短暂持有
DRM master。设备 DRM 后端在初始化 KMS 前最多等待十秒以获取 master；首次 atomic
modeset 在提交成功前保持可重试，DRM 告警与错误会转发到服务日志。

### 使用 LPM 构建 K230 Debian riscv64

Linux 主机安装 `gcc-riscv64-linux-gnu` 和 `g++-riscv64-linux-gnu`，macOS 主机用 `brew install llvm lld dpkg` 安装交叉编译和打包工具。在应用仓库根目录运行打包命令。

```sh
lpm pack --arch riscv64
```

`k230-debian` CMake 预设首次配置时从 `LILYGO-UI/canaan-gen` Release 下载固定版本的 Debian BSP，校验 SHA-256，并将压缩包和 `sysroot/` 保存在当前项目的 `.cache/k230-bsp/20261116T104343Z/`。之后可以离线打包，重新生成 `build/` 也会复用缓存。生成的 `.deb` 在 `dist/` 目录。

执行一次 `lpm set-target k230` 后，后续直接运行 `lpm pack`。如需自定义 Debian sysroot，在首次配置前设置 `LILYGO_UI_DEBIAN_SYSROOT`，或向 CMake 传入 `-DLILYGO_UI_DEBIAN_SYSROOT=/path/to/sysroot`。AppKit 默认使用 `third_party/cm0-appkit`，可用 `-DLilyGoUI_DIR=/path/to/appkit` 指定其他版本。
