# LILYGO UI 设计文档

这里按设计主题维护 Launcher 与平台集成的目标、方案、约束和验证要求。CM0 是首个
产品配置；应用身份和仓库边界遵循根目录的 [项目约定](../AGENTS.md)。

## 阅读导航

| 文档 | 解决的问题 |
| --- | --- |
| [应用生命周期与显示交接](launcher-app-lifecycle.md) | Launcher 如何发现和启动独立应用，交接显示与输入，恢复首页，以及通过独立服务完成自身升级 |
| [图形渲染](graphics-rendering.md) | 软件、OpenGLES、NanoVG 的选择依据，旋转与缓存管理，私有 Mesa 和软件整帧缓冲的边界 |
| [SDK 与打包](sdk-packaging.md) | 独立仓库如何消费源码 SDK、静态链接 AppKit/LVGL，共享字体并独立发布 Debian 包 |
| [UI 设计规范](ui-design-guidelines.md) | 响应式布局、颜色、字体、字号、中西文混排与字体授权 |

从 [中文 README](../README.zh-CN.md)（[English](../README.md)）开始构建和运行；
板端复现步骤、测试参数和测量口径见
[DRM 硬件测试指南](../tests/README.md)。AppKit 内部实现和工具分别在其
[文档目录](../third_party/cm0-appkit/docs/) 与 [README](../third_party/cm0-appkit/README.md)
维护，本目录描述 Launcher 对它们的使用与约束。

## 当前设计基线

- 应用独立构建、测试和打包，通过 manifest 被 Launcher 发现；每次启动都是独立进程。
- AppKit 与 LVGL 从固定源码子模块静态编译；`lilygo-ui-appkit-dev` 提供源码 SDK、
  共享运行时字体和许可证，应用不依赖 AppKit/LVGL 共享库。
- 设备直接使用 DRM/KMS 与 evdev。SDK 默认软件渲染；CM0 的 VC4 优化方案显式选择
  OpenGLES。NanoVG 保持实验状态，设备实际配置以构建产物与启动日志为准。
- 支持交接的 DRM/EGL 路径保留 Launcher UI、上下文与最后一帧；软件 DRM 或交接
  不可用时完整重建。两条路径都必须覆盖返回与异常恢复。

## 设计与证据的维护

设计文档使用无日期的主题文件名，描述当前应遵守的行为。修改实现影响架构、接口、
资源所有权或失败恢复时，同步更新对应设计及验证要求。新决策应交代适用范围、
选择依据和被替代方案，避免把一次部署的版本、IP 或目录当作长期接口。

[历史记录索引](archive/README.md) 保存按日期归档的优化、基准和部署记录，供追溯
决策与回退材料使用。历史测量只证明当次构建和场景，不能证明后续代码已重新验证；
其中的包版本、设备状态与命令均按当时上下文解读。`build/`、`dist/` 和设备上的
原始日志、包及备份通常未纳入仓库，文中路径不保证在其他 checkout 中存在。
