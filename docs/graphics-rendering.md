# 图形渲染与缓冲管理设计

本文定义 Launcher 与 AppKit 的渲染器选择、旋转、输入和图形资源约束，以及 CM0
上的验证方法。应用进程与显示设备的交接流程见
[Launcher 生命周期设计](launcher-app-lifecycle.md)，源码 SDK 和软件包边界见
[SDK 与打包设计](sdk-packaging.md)。

## 目标与适用范围

设备端通过 DRM/KMS 直接显示，Launcher 和前台应用按顺序取得显示设备所有权。
设计需要同时满足横竖屏正确显示、触摸事件完整、重复切换后资源可恢复，并控制
CPU、启动耗时和内存占用。渲染速度的改善必须以文字、颜色、裁剪、透明度和形状
正确为前提。

`LILYGO_UI_RENDERER` 是编译期选项，默认值为 `software`。面向 CM0 VC4 的 GPU
优化方案选择 `opengles`；`nanovg` 保留为实验选项。这个选择不表示每个已部署的
程序都使用 OpenGLES：2026-09-22 的返回缓冲故障记录对应软件渲染 Launcher。
判断设备实际配置时，应核对程序的渲染器日志、EGL renderer 和进程库映射。

AppKit 负责 LVGL、DRM/EGL 和输入集成；Launcher 负责显示会话和前台应用调度。
独立应用各自编译并静态链接 AppKit/LVGL，通过自己的显示会话运行，不共享
Launcher 的 LVGL 对象、字体对象或 GPU 纹理。私有 Mesa 的构建和打包属于独立
运行库仓库。视频硬件编解码不属于本设计的 UI 渲染路径。

## 渲染器选择

| 选项 | 绘制与显示方式 | 适用场景和约束 |
| --- | --- | --- |
| `software` | CPU 绘制；DRM 竖屏直接使用 scanout 缓冲，旋转时增加整帧中间缓冲 | 默认兼容方案；固定 BSP 可直接使用；DRM 应用切换采用显示重建 |
| `opengles` | LVGL 软件绘制生成未缓存内容，GLES2 缓存纹理并完成合成、旋转；EGL/GBM 提交到 DRM | VC4 优化方案；降低适合缓存场景的 CPU 开销；保留软件绘制能力，不能据此认定全部控件都由 GPU 绘制 |
| `nanovg` | NanoVG GLES2 直接绘制图元并合成文字、图像，按整帧渲染 | 实验方案；需要 stencil-8、4x MSAA 的 EGL window 配置；阴影和圆弧仍存在已知视觉差异 |

GPU 构建需要目标系统或交叉 sysroot 提供 GBM、EGL 和 GLES2 开发库，设备还必须
具有可工作的硬件驱动。固定 BSP 0.1.0 未包含所需 GPU 开发库，应使用补齐依赖的
独立 sysroot。每种渲染器使用独立构建目录，避免复用不匹配的 LVGL 补丁和缓存。
SDL 模拟器仅使用软件渲染；fbdev 保留为兼容诊断后端。

渲染器不能在 EGL 初始化失败后自动切换为另一个 DRM 渲染器。初始化失败会报错，
需要选择适合目标设备的构建。OpenGLES 和 NanoVG 的显示交接失败可以退回完整
重建，这与切换成软件渲染是两件事。

NanoVG 暂不作为 VC4 的默认优化方案，原因是动态场景中的宽灰色阴影带和圆弧碎片
与软件/OpenGLES 参考图不一致。其子离屏层使用单采样 FBO，不继承 window MSAA；
渐变仅支持两种颜色。图像和字形缓存上限分别为 128、512 项，这是条目数而非内存
预算。启用它之前必须检查嵌套透明度、裁剪、文字和复杂离屏层，并重新测量内存。

## 显示旋转、输入与缓存

CM0 验证设备的 DSI 面板原生尺寸为 568×1232、刷新率 35 Hz。测得的主平面支持
0/180 度旋转和反射，不支持 90 度旋转，因此横屏由渲染路径完成。connector ID、
plane ID 和可用模式是设备诊断结果，不应作为所有产品的固定接口。

OpenGLES 在逻辑画布中完成纹理合成，再将画面旋转到原生显示坐标；软件 DRM 将
完整逻辑帧旋转写入 scanout 缓冲。NanoVG 的根层绘制和裁剪使用原生窗口坐标，
子离屏层保持逻辑坐标。SDL 模拟器通过交换窗口宽高呈现横屏预览。

当前 Launcher 横屏使用 `LV_DISPLAY_ROTATION_90`，AppKit 应用运行时使用
`LV_DISPLAY_ROTATION_270`。测试必须与被测构建的实际旋转配置一致，尤其是底部
上划退出的输入坐标；不能从 Launcher 的旋转值推断应用值。方向未改变时，不重复
调用 `lv_display_set_rotation()`，避免 LVGL 再次发出分辨率事件并触发纹理重建。

evdev 按每个完整的指针 `SYN_REPORT` 向 LVGL 提交一次采样。冷帧或显示重建可能
延迟轮询，此时队列中的按下和抬起仍须分别交付，避免一次短点击被折叠为只有抬起。
输入重开之后，调用方必须重新绑定手势回调。

图形资源遵循以下生命周期约束：

- DRM/EGL 交接前停止绘制并完成未结束的 page flip，保留最后呈现的 GBM buffer
  锁和 EGL/纹理资源；交接期间不继续绘制。完整的暂停、恢复和失败路径由
  [显示与输入所有权](launcher-app-lifecycle.md#显示与输入所有权)规定。
- 销毁 EGL context 前，在原 context 仍为 current 时释放其 GPU 资源。重新创建
  显示时重建 context 相关缓存；NanoVG 只保留一个已注册 draw unit，避免重复注册。
- NanoVG 获取 FreeType 字形时使用局部 descriptor，不修改作为缓存红黑树 key 的
  descriptor；按 API 释放字形数据和缓存引用。缓存压力触发 flush 后保留当前帧
  状态和变换，使该绘制任务余下的命令仍能提交。
- 保留 Launcher 缓存会增加前台应用运行时的总占用。评估内存时同时测量 Launcher、
  应用和系统可用内存；8 MiB LVGL heap 不包含所有字体、驱动和 GPU 分配。

## 软件旋转缓冲的内存设计

软件 DRM 显示创建时为旋转准备一块独立整帧缓冲，大小按像素格式对应的行跨度乘
原生高度计算。在 568×1232、每像素 4 字节时，大小为 2,799,104 字节。
该缓冲通过匿名 `mmap(MAP_PRIVATE | MAP_ANONYMOUS)` 分配，不进入 AppKit 的
固定 8 MiB LVGL TLSF 内存池。

竖屏使用两个 DRM scanout 缓冲的 direct 路径，不写入旋转中间缓冲。非零旋转时
切到整帧渲染模式，flush 使用 `lv_draw_sw_rotate()` 将逻辑帧写入当前 scanout
缓冲；切回竖屏后恢复 direct 路径。匿名映射页在旋转路径实际写入时才提交物理内存。
显示销毁时，驱动使用 `munmap()` 释放映射；映射失败时清空指针、记录 errno，
返回初始化失败并沿既有清理路径释放已取得的资源。

池外分配用于隔离大块短生命周期缓冲与长生命周期字体、图像缓存。原实现每次
重建显示都向 LVGL 池申请整帧连续空间，即使总空闲内存仍足够，缓存分配造成的
碎片和 TLSF 分桶规则也可能使申请失败。2026-09-22 的诊断在第 4 次重建复现失败：
总空闲约 8.35 MB、最大块 2,800,576 字节，而该请求搜索的桶下限为 2,818,048 字节。
这说明仅增加总空闲量或通过短期切换测试，不能证明连续分配可靠。

这项设计消除整帧缓冲对 LVGL 连续大块的依赖，不消除系统内存耗尽的可能。回归测试
需要覆盖多次重建、90/180/270 度旋转、切回竖屏、映射失败和部分初始化失败的清理。
已有测试在受限 LVGL 分配器下执行 64 次重建；历史设备验证覆盖四个应用、横竖屏
各 10 次返回，共 80 次。完整故障证据见
[软件返回缓冲修复记录](archive/launcher-return-buffer-fix-2026-09-22.md)。

## 私有 Mesa VC4 运行库

私有 `lilygo-ui-mesa-vc4` 针对 VC4、DRM 和 GLES2 提供配套 GBM/EGL/GLES2/Gallium
库。它与系统 Mesa 并存，由产品镜像或服务配置决定哪些进程使用；Launcher 启动的
子应用继承服务环境。AppKit 和 Launcher 不承担 Mesa 源码构建或运行库打包。

该方案针对应用进入时的驱动加载成本。2026-09-17 的分段计时发现
`gbm_create_device()` 耗时约 150–1400 ms，通用 Gallium 引入 LLVM、Z3 及大量
重定位。验证使用未修改的上游 Mesa 26.2.0，移除 LLVM、Z3、其他 GPU、X11、
Wayland 等未使用能力，私有安装体积约 17 MiB。通用依赖文件体积约 194 MiB
只是磁盘数据，不能当作进程常驻内存。

启用时为所需服务设置以下路径；手动测试进程不会自动继承 systemd 服务环境：

```ini
[Service]
Environment="LD_LIBRARY_PATH=/usr/lib/lilygo-ui/mesa-vc4/lib"
Environment="GBM_BACKENDS_PATH=/usr/lib/lilygo-ui/mesa-vc4/lib/gbm"
```

这个运行库不提供通用桌面 OpenGL、X11、Wayland 或视频编解码能力。接入前核对
服务及子应用的图形需求，并通过进程映射确认实际加载的库。构建、源码锁定、ELF
依赖检查和镜像接入方式由
[独立 Mesa VC4 仓库](https://github.com/LILYGO-UI/mesa-vc4)维护。引入私有运行库的
历史收益与部署证据见
[应用切换优化记录](archive/app-switch-latency-2026-09-17.md)；切换计时定义统一见
[Launcher 生命周期设计](launcher-app-lifecycle.md#切换耗时诊断)。

## 性能依据与验收口径

渲染器选择依据同一硬件、场景和动画时刻的对比。2026-09-08 的基线验证表明
OpenGLES 在滚动列表中能降低 CPU 开销，代价是增加进程内存。2026-09-17 扩展为
列表和动态控件两类场景、两种方向、三种渲染器，各运行三轮显示创建/销毁，每轮
预热 1 秒并测量 8 秒。下表摘录两个代表场景，CPU 以单个满载核为 100%：

| 场景与方向 | 指标 | Software | OpenGLES | NanoVG |
| --- | --- | ---: | ---: | ---: |
| 列表横屏 | CPU | 70.51% | 22.21% | 15.85% |
| 列表横屏 | 平均 LVGL refresh | 32.28 ms | 7.04 ms | 4.73 ms |
| 动态竖屏 | CPU | 91.24% | 72.25% | 21.76% |
| 动态竖屏 | 平均 LVGL refresh | 88.95 ms | 23.17 ms | 5.70 ms |
| 动态竖屏 | 进程峰值 RSS | 21.16 MiB | 79.69 MiB | 82.96 MiB |

NanoVG 动态竖屏 CPU 比 OpenGLES 低约 70%，但图像不等价，因此该数字表示继续
优化的潜力，不能作为通过视觉验收的性能提升。上述测试使用当时的通用 Mesa，
后续私有 VC4 运行库测试中的 RSS 约为每进程 26–27 MiB；跨批次比较必须同时核对
驱动、程序、工作负载和采样方式，不能将这些数值直接当作当前内存预算。

后续修改按以下口径验收：

1. **先确认配置与画面。** 记录二进制哈希、渲染器、Mesa 实现、面板模式和旋转；
   在列表与动态场景、横竖屏下检查字体、颜色、透明度、阴影、圆弧和裁剪。固定
   `--snapshot-ms 1000` 可使动画截图具有相同时间基准。
2. **再比较同步渲染成本。** 使用同一设备、编译优化、时钟/电源策略及后台负载，
   按测量时长或帧数加权聚合 CPU 和 refresh 指标。LVGL refresh 完成不等于异步
   GPU 工作结束，也不等于物理 scanout 完成；不得将其直接称为屏幕 FPS。
3. **单独验证生命周期与内存。** 重复创建/销毁、真实应用返回、失败退出和完整
   重建必须通过。峰值 RSS 包含进程启动和缓存，但不覆盖完整 GPU 内存；就绪后的
   RSS、系统 MemAvailable 和全程峰值不能混为同一种指标。
4. **保留测量边界。** 合成逻辑帧截图不证明实际面板旋转正确，软件首帧和日志
   就绪标记不证明输入到显示延迟。物理显示、触摸到显示和长期稳定性需另行验证。

历史测量是架构决策依据，不验证后续代码。完整数据、环境、备份和原始材料位置见
[2026-09-08 图形验证](archive/graphics-validation-2026-09-08.md)及
[2026-09-17 三渲染器对比](archive/graphics-validation-2026-09-17.md)。

## 实现与验证入口

| 职责 | 入口 |
| --- | --- |
| 渲染器选项、依赖与补丁接入 | [AppKit 构建配置](../third_party/cm0-appkit/CM0AppKit.cmake)、[LVGL 配置](../third_party/cm0-appkit/config/lv_conf.h) |
| Launcher 显示所有权与方向 | [DisplaySession](../src/display_session.cpp) |
| 应用方向与运行时初始化 | [AppKit Runtime](../third_party/cm0-appkit/src/runtime.cpp) |
| EGL/GBM 与交接实现 | [DRM/EGL 补丁](../third_party/cm0-appkit/patches/lvgl-9.5.0-drm-egl.patch)、[DRM 交接补丁](../third_party/cm0-appkit/patches/lvgl-9.5.0-drm-handoff.patch) |
| 软件旋转缓冲 | [软件 DRM 补丁](../third_party/cm0-appkit/patches/lvgl-9.5.0-drm-software-rotation.patch)、[缓冲回归测试](../third_party/cm0-appkit/tests/test_drm_software_buffer.c) |
| NanoVG 生命周期和缓存 | [NanoVG 集成补丁](../third_party/cm0-appkit/patches/lvgl-9.5.0-nanovg-drm.patch)、[生命周期](../third_party/cm0-appkit/tests/test_nanovg_lifecycle.c)、[字形缓存](../third_party/cm0-appkit/tests/test_nanovg_glyph_cache.c)、[缓存压力](../third_party/cm0-appkit/tests/test_nanovg_pending.c) |
| evdev 采样完整性 | [evdev 补丁](../third_party/cm0-appkit/patches/lvgl-9.5.0-evdev-sync.patch)、[输入回归测试](../third_party/cm0-appkit/tests/test_evdev_sync.c) |
| 板端配置与性能采样 | [图形诊断指南](../third_party/cm0-appkit/docs/graphics-diagnostics.md)、[可重复渲染基准](../third_party/cm0-appkit/docs/graphics-benchmark.md) |
| Launcher 主机与板端交接验收 | [DRM 硬件测试指南](../tests/README.md) |

板端测试需独占 DRM，按测试指南先停止现有服务，测试结束后恢复服务。主机 mock
测试覆盖资源所有权与错误路径，板端生命周期测试覆盖实际驱动和进程交接，两者均
不能替代视觉对比。LVGL 补丁更新后，应使用新构建目录或重新解包 LVGL，避免测试
仍使用已缓存的旧补丁源码。
