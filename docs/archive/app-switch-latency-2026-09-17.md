> 历史记录：本文保留当次优化、验证或部署的上下文，不代表当前代码或设备状态。
> 当前设计见 [应用生命周期与显示交接设计](../launcher-app-lifecycle.md)、[图形渲染设计](../graphics-rendering.md)；[返回归档索引](README.md)。

# CM0 应用切换等待优化

本文记录 2026-09-17 的部署及硬件验证结果。后续代码的复现步骤和计时口径见
[DRM 硬件测试说明](../../tests/README.md)；本文数据不代表后续修改已重新上板验证。

已在 `192.168.8.206` 部署新的 Launcher、设置、文件管理、Meshtastic 和私有 VC4
运行库。显示仍采用 DRM/KMS、EGL/GBM 和 OpenGLES；NanoVG 保持实验选项。

## 实测结果

下表为横竖屏合并的中位数，单位 ms。基线每个应用 4 次，最终方案每个应用 6 次。
“进入”从模拟点击抬起开始，到 AppKit 完成首次 `lv_refr_now()` 后输出就绪标记；
“返回”从向应用发送 SIGTERM 开始，到 Launcher 输出恢复就绪标记。

| 应用 | 进入：优化前 | 进入：优化后 | 返回：优化前 | 返回：优化后 |
| --- | ---: | ---: | ---: | ---: |
| 设置 | 837 | 333 | 842 | 88 |
| 文件管理 | 784 | 220 | 659 | 81 |
| Meshtastic | 487 | 253 | 546 | 72 |

这些是软件就绪标记的观测时间，日志轮询间隔 10 ms，不是光学输入到显示延迟。
Files 使用临时空目录，Meshtastic 使用回环端口，XDG 配置隔离；网络、文件数量和
其他系统负载可能改变实际启动时间。另行验证了真实底部上滑退出。

## 原因及修改

**返回时重建整个图形环境。** 原实现每次关闭输入设备、删除 Launcher UI 和 EGL
上下文，返回时重建全部控件、纹理缓存，并刷新两次。新实现暂停绘制、完成未结束
的 page flip，然后释放 DRM master，同时保留最后一帧、UI、EGL 和 GBM 缓冲。
应用退出后重新获得 master，直接呈现保留帧并重新打开输入设备、绑定下拉手势。
软件 DRM 或交接失败仍可使用完整重建路径。

**新应用加载通用 Mesa 的成本较高。** 分段计时将主要耗时定位到
`gbm_create_device()`，观察到约 150–1400 ms。系统 Gallium 库依赖 LLVM 和 Z3；
它们的文件大小合计约 194 MiB，并有大量动态重定位。文件大小不代表常驻内存。
采用未修改的上游 Mesa 26.2.0，构建完整、私有的 VC4 GBM/EGL/GLES2/Gallium
运行库，去掉 LLVM、Z3、其他 GPU、X11、Wayland 和不需要的功能。安装体积约
17 MiB，系统 Mesa 保留。只给 Launcher 服务及其子应用设置私有库路径。

**其他已确认的等待。** SDK 和 Launcher 不再在方向未变化时重新分配整屏纹理。
Meshtastic 连接等待改为每 100 ms 检查取消，保留总连接期限；退出时不再等待可能
阻塞的断开消息写入，网络发送不再持有控制锁。真实回环 TCP 回归测试覆盖接收端
不读数据、连接持续等待及关闭/重新启用连接；退出耗时约 0–40 ms。DNS 查询仍使用
同步 `getaddrinfo()`，默认数字 IP 不涉及域名解析。

仅保留 Launcher 资源时，返回已变快，但进入仍受通用 Mesa 加载影响。最终加入
私有驱动后，两段耗时都改善。切换测试期间，Launcher 和应用各自的 RSS 从约
58–75 MiB 降到约 26–27 MiB；这不是完整 GPU 内存统计。系统还有其他服务与 zram，
不同测试批次的 MemAvailable 不能直接比较；最终批次观察到的最低值约 55 MiB。

## 验证

- Launcher 7 项、SDK 8 项 CTest 通过；包含输入设备重建后的下拉手势回归。
- DRM 交接测试覆盖保留 framebuffer、有限等待、失败回退、暂停销毁及连续 20 次交接。
- Meshtastic 核心测试及真实 TCP 停止回归通过，旧实现会触发阻塞发送超时断言。
- 最终私有驱动完成三应用、横竖屏各三次启动与正常退出，共 18 次。
- 三应用横竖屏底部上滑退出通过。
- 子进程退出码 1、被信号终止及 exec 失败后均能恢复 Launcher，各测试三次。
- 强制完整重建路径启动、返回及横屏退出通过。
- 动态横屏基准的 2,099,328 个 RGB 字节与原驱动截图完全一致。
- 实际打包 ELF 均为 ARM64、无构建 RPATH、无临时 profiling 代码，字体许可证完整。
- 安装后服务为 active/running，NRestarts=0。检查实际进程映射确认加载私有
  Gallium 26.2.0，没有加载 LLVM 或 Z3。

## 部署与回退

私有运行库包为 `lilygo-ui-mesa-vc4_26.2.0_arm64.deb`。原有四个应用包保持开发版本
0.1.0；板上的 AppKit 1.2.0 和共享字体未替换。库路径仅通过以下文件激活：

```text
/etc/systemd/system/lilygo-ui-launcher.service.d/50-vc4-runtime.conf
```

```ini
[Service]
Environment="LD_LIBRARY_PATH=/usr/lib/lilygo-ui/mesa-vc4/lib"
Environment="GBM_BACKENDS_PATH=/usr/lib/lilygo-ui/mesa-vc4/lib/gbm"
```

这个私有运行库面向 VC4、DRM 和 GLES2，不提供通用桌面 OpenGL、X11、Wayland
或视频编解码功能。服务启动的图形应用必须适合该配置；从其他会话直接启动程序
仍默认使用系统 Mesa。

部署前 32 个软件包所属文件已备份到目标板：

```text
/var/lib/lilygo-ui-launcher/switch-backup-20260917/installed-files.tar.gz
```

只回到系统 Mesa，可移走私有运行库的服务配置并重新加载服务：

```sh
sudo mv /etc/systemd/system/lilygo-ui-launcher.service.d/50-vc4-runtime.conf \
  /var/lib/lilygo-ui-launcher/switch-backup-20260917/50-vc4-runtime.conf.disabled
sudo systemctl daemon-reload
sudo systemctl restart lilygo-ui-launcher
```

需要同时恢复旧程序和资源时，先停止服务，再解压备份到 `/`，之后启动服务。
这只恢复原有文件，不回退 Debian 依赖记录，也不操作用户应用数据。私有 Mesa
软件包可以保持安装但不启用。`LILYGO_UI_DRM_RECREATE=1` 可用于诊断完整重建路径。

## 复现和证据

- 基线、分段 profiling、最终计时、内存、截图和部署日志：`build/switch-results/`。
- 最终应用与驱动包：`build/switch-packages/`。
- Mesa 原始构建、ELF 检查和版本记录：`build/mesa-vc4/`。
- 目标板临时验证文件：`/tmp/lilygo-switch-validation/`。

[独立 Mesa VC4 仓库](https://github.com/LILYGO-UI/mesa-vc4#readme)维护源码锁定、构建与 DEB
打包工具，并验证 ELF、动态依赖、版本要求及许可证。它不依赖 Launcher 或 AppKit
工程；服务启用由产品镜像配置负责。上述包名与构建目录保留为当时的验证记录。
