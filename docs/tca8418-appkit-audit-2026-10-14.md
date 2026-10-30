# TCA8418 AppKit 兼容性审计（2026-10-14）

中央 AppKit 当前工作树基线：`67254609f488d1a4fb407b1544186f92b63320ff`。当前工作树包含统一键盘运行时、严格 TCA8418 设备契约、焦点 scope 与回归测试；由于执行环境不允许写入 AppKit `.git`，这些改动尚未形成新的可 pin commit。

## 结论

- 26 个带 AppKit 的 UI 应用中，24 个是 Git 子模块，2 个是无 Git 元数据的嵌入副本。`lilygo-ui-mesa-vc4` 是独立 Mesa 组件，不纳入统计。
- 初始快照中 20 个子模块基于 `100d55b` 并携带相同的未提交键盘改动，3 个基于 `6725460` 并携带相同改动，Gallery 是干净但落后的 `6725460`。父仓库 gitlink 与实际 checkout 普遍不一致。
- Bilibili 和 Hongguo 的嵌入副本没有 `include/cm0/input.h`/`src/input.cpp`；Hongguo 还自带第二套 Linux evdev 键盘实现。这两项不能通过简单刷新 Git 子模块解决。
- 当前已将中央工作树内容同步到 24 个可写 Git 子模块的工作树；这使源码内容一致，但没有更新父仓库 gitlink，也没有生成可重现的发布提交。

## 统一规范

1. Linux `tca8418_keypad` 驱动负责矩阵扫描和 device-tree `linux,keymap`；应用不得读 TCA8418 寄存器、矩阵或打开第二个 event 节点。
2. AppKit 是唯一输入探测和映射层。运行时按 `EVIOCGNAME` 识别名称中的 `tca8418`，并要求 Enter、Escape、Tab、Backspace、Delete、四向箭头、Home、End、A-Z、0-9、Space 能力位。
3. AppKit 负责热插拔、key repeat、默认 focus group、方向导航、焦点环、modal focus scope 和硬件键盘连接时的软件键盘隐藏。
4. 应用只注册业务级 `LV_EVENT_KEY`；游戏/终端临时接管必须保存并恢复 group，处理 stale key-up 和 input-device delete，不能替换 AppKit 的 Esc/Enter 语义。
5. Sensors 的 keyboard 页面只能做诊断；NES 触摸 reader 与 Xiaozhi 麦克风按键属于其他输入源，不得误判为 TCA8418 主输入。

## 应用状态（同步后的工作树）

| 应用 | 嵌套 AppKit HEAD | 父仓库 gitlink |
|---|---|---|
| `lilygo-ui-bilibili` | `embedded-cop` | `-` |
| `lilygo-ui-calculator` | `100d55b7a5fa` | `f3a825bd4437` |
| `lilygo-ui-camera` | `100d55b7a5fa` | `e88f3b3ab079` |
| `lilygo-ui-cellular` | `100d55b7a5fa` | `e88f3b3ab079` |
| `lilygo-ui-clash` | `100d55b7a5fa` | `e88f3b3ab079` |
| `lilygo-ui-files` | `67254609f488` | `67254609f488` |
| `lilygo-ui-gallery` | `67254609f488` | `f3a825bd4437` |
| `lilygo-ui-hongguo` | `embedded-cop` | `-` |
| `lilygo-ui-i2cdetector` | `100d55b7a5fa` | `f3a825bd4437` |
| `lilygo-ui-imu` | `100d55b7a5fa` | `f3a825bd4437` |
| `lilygo-ui-maps` | `100d55b7a5fa` | `f3a825bd4437` |
| `lilygo-ui-meshtastic` | `100d55b7a5fa` | `f3a825bd4437` |
| `lilygo-ui-monitor` | `100d55b7a5fa` | `f3a825bd4437` |
| `lilygo-ui-music` | `100d55b7a5fa` | `e88f3b3ab079` |
| `lilygo-ui-nes` | `67254609f488` | `67254609f488` |
| `lilygo-ui-picoclaw` | `100d55b7a5fa` | `e88f3b3ab079` |
| `lilygo-ui-radio` | `100d55b7a5fa` | `f3a825bd4437` |
| `lilygo-ui-sensors` | `100d55b7a5fa` | `f3a825bd4437` |
| `lilygo-ui-serial` | `100d55b7a5fa` | `e88f3b3ab079` |
| `lilygo-ui-settings` | `100d55b7a5fa` | `e88f3b3ab079` |
| `lilygo-ui-store` | `67254609f488` | `67254609f488` |
| `lilygo-ui-telegram` | `100d55b7a5fa` | `e88f3b3ab079` |
| `lilygo-ui-template` | `100d55b7a5fa` | `e88f3b3ab079` |
| `lilygo-ui-terminal` | `100d55b7a5fa` | `f3a825bd4437` |
| `lilygo-ui-xiaozhi` | `100d55b7a5fa` | `f3a825bd4437` |
| `lilygo-ui-youtube` | `100d55b7a5fa` | `f3a825bd4437` |

## 发布前闸门

提交中央 AppKit 后，必须把所有 24 个 Git 子模块的父仓库 gitlink 更新到同一 canonical commit，并为 Bilibili/Hongguo 删除嵌入副本、改为标准 `third_party/cm0-appkit` 子模块或明确将其迁移出 CM0 UI 发布集合。运行 `tools/check_appkit_compatibility.py --strict`，再对每个应用执行 host build/test；最后在目标板上验证真实 TCA8418 拔插、Esc/Enter、Shift/Caps、Tab/方向导航和 modal cancel。

同步中央工作树后的复核结果为：27 个项目、25 个工作树内容一致、25 个 dirty、2 个无 Git 元数据嵌入副本、0 个 clean pinned canonical。剩余失败项是可重现性问题（父仓库 gitlink 尚未提交）以及 Bilibili/Hongguo 的旧嵌入副本；它们不能靠继续复制工作树来解决。
