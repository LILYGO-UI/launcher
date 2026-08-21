# LILYGO UI 设计规范

本文规定 LILYGO UI Launcher、AppKit 以及独立 LILYGO UI 应用的设计要求，统一响应式
布局、字体、字号和中西文混排规则；新增或修改界面时必须遵守本文。

## 字体家族

| 文本内容 | 字体 | 说明 |
| --- | --- | --- |
| 西文字母、数字和西文标点 | [Inter](https://rsms.me/inter/) | 默认使用 Regular 字重 |
| 简体中文及中文标点 | [思源黑体](https://fonts.adobe.com/fonts/source-han-sans-simplified-chinese)（Source Han Sans CN） | 默认使用 Normal 字重 |

包含中西文的同一段文本必须直接使用 AppKit 提供的复合字体，或使用以该复合字体
作为 fallback 的应用增量字体。AppKit 复合字体以 Inter 显示西文字母、数字和西文
标点，以思源黑体显示中文，并统一基线、字号和 fallback。不要为了混排而拆分标签，
也不要依赖操作系统的默认字体回退。

图标字形使用 AppKit 内嵌的 Font Awesome 字形，不改变正文的字体选择规则。

## 字号层级

只使用 AppKit 已提供的以下字号，不在应用内建立另一套字号或直接引用字体文件：

| 字号 | LVGL token | 推荐用途 |
| --- | --- | --- |
| 14 px | `CM0_FONT_UI_14` | 辅助信息、紧凑标签 |
| 22 px | `CM0_FONT_UI_22` | 正文、按钮、列表项和状态栏 |
| 28 px | `CM0_FONT_UI_28` | 小标题、重点数值 |
| 36 px | `CM0_FONT_UI_36` | 页面标题、主要数值 |
| 48 px | `CM0_FONT_UI_48` | 时间、结果等需要突出显示的内容 |

相同信息层级在不同页面中应使用相同字号。缩小字号不能作为修复溢出的首选方案；
应先精简文案、调整布局或按产品要求使用截断和换行。

## 响应式布局

新增或修改 UI 时必须使用响应式设计技术，使同一套界面能够适应目标设备的不同
显示尺寸、宽高比和横竖屏方向。不得只针对单一固定分辨率排列控件，也不得通过
复制整套页面来分别维护横屏、竖屏、phone 或 desktop 布局。

LVGL 界面应优先使用 Flex 或 Grid layout 组织控件，并结合百分比尺寸、
`LV_SIZE_CONTENT`、最小/最大尺寸、对齐、间距和滚动容器分配可用空间。固定像素值
只用于有明确设计约束的尺寸，例如触控目标下限、图标尺寸、边框和设计 token；
不得依赖大量绝对坐标实现页面主体布局。

连续的尺寸变化应由 layout 自动处理。导航方向、栏数、内容优先级或交互方式发生
结构性变化时，应根据容器的实际可用宽高设置明确断点，并在尺寸变化后重新应用布局。
断点依据显示几何、输入方式和会话能力确定，不能只依据 CPU 架构或设备型号。
可复用组件必须以父容器的可用尺寸为响应依据，不能假设自己始终占满 display。
运行时尺寸或方向变化触发布局更新时，更新过程必须可重复执行，并保留当前业务状态、
焦点、滚动位置和用户输入；不得把重建整个页面作为常规的尺寸适配方式。

响应式布局必须保证：

- 文本按产品要求换行或截断，不与相邻内容重叠，也不通过任意缩小字号来适配；
- 主要操作在各目标尺寸下始终可见、可达，并保持足够的触控或指针命中区域；
- 内容增多、文案变长或字体回退时，容器能够扩展、重排或滚动，不发生裁切；
- phone、desktop、横屏和竖屏可以改变呈现方式，但保持同一应用身份和业务状态；
- 在支持的最小、典型和最大视口以及横竖屏切换后完成渲染与交互验证。

## LVGL 实现

LVGL 代码必须包含 `<cm0/typography.h>` 并使用公共字体 token：

```c
#include <cm0/typography.h>

lv_obj_set_style_text_font(label, CM0_FONT_UI_22, 0);
```

不得在公共 UI 中新增 `lv_font_montserrat_*` 引用。通常应直接使用 AppKit token，
这样可以保证 Launcher 与各应用的字形、基线和 fallback 一致。仅当独立应用使用了
AppKit 子集未包含的应用专有字形时，才可按“应用专有字形”一节创建私有增量字体。

Launcher、AppKit 和独立应用都直接使用 LVGL API 创建界面。View 代码必须直接引用
上述 `CM0_FONT_UI_*` token，或引用以同字号 token 为 fallback 的应用增量字体；不能
复制、改名或重新生成一份完整的 AppKit 字体数据。

## 应用专有字形

独立应用的文案可能包含 AppKit 裁剪字符集没有、且其他应用不需要的字形。此时应用
可以在自己的仓库中生成只包含缺失字形的私有字体，并把它放在 fallback 链首；相同
字号的 AppKit 字体必须作为 fallback。不要把 AppKit 已有的西文、中文或图标字形
重复编入私有字体。

`fallback` 是 LVGL 字体规范中 `lv_font_t` 的原生字段，不是 AppKit 定义的字体机制。
`lv_font_conv` 生成的字体对象是只读的，且生成文件中的 `.fallback` 默认为 `NULL`。
不要手改生成文件；应用应复制生成的字体描述符，再按 LVGL 规范设置 fallback：

```c
#include <cm0/typography.h>

LV_FONT_DECLARE(my_app_extra_22)

static lv_font_t my_app_ui_22;

static void init_fonts(void)
{
    my_app_ui_22 = my_app_extra_22;
    my_app_ui_22.fallback = CM0_FONT_UI_22;
}

/* Call init_fonts() before creating the label. */
lv_obj_set_style_text_font(label, &my_app_ui_22, 0);
```

私有字体必须使用相同字号和字体家族：中文使用 Source Han Sans CN，额外西文使用
Inter。复制后的 `lv_font_t` 必须至少与引用它的 LVGL 对象存活时间相同，因此应放在
静态存储或应用状态对象中，不能使用创建函数内的栈变量。每个需要额外字形的字号都
要生成对应的增量字体，并 fallback 到同字号的 `CM0_FONT_UI_*` token。

## Ardot 实现

Ardot 中必须使用准确的字体家族名称 `Inter` 和 `Source Han Sans CN`。不含 CJK
字符的文本选择 `Inter`；包含中文的文本选择 `Source Han Sans CN`。字号应与上表
的层级保持一致。

## 字符集与生成

设备端内嵌的是经过裁剪的字体，不是完整 CJK 字库。新增中文文案时，先判断该字符
是平台通用字形还是单个应用的专有字形：

1. 检查字符是否已列入 AppKit 仓库的 `fonts/glyphs.txt`；
2. 平台、Launcher 或多个应用都会使用的字符应补入该文件，并按
   AppKit 仓库的 `fonts/README.md` 重新生成所有受影响的字号；
3. 仅单个独立应用使用的字符应留在应用仓库的私有增量字体中，并按上一节设置
   AppKit fallback；
4. 在设备构建或 UI 渲染测试中确认增量字形和 AppKit 字形均能正确解析，且没有
   缺字方框、错误回退或文字裁切。

未经存储预算评审，不得内嵌完整 CJK 字体。

## 授权与分发

Inter、思源黑体和内嵌的 Font Awesome 字形均按 SIL Open Font License 1.1
分发。它们是受版权保护的 OFL 授权资产，不能描述为“无版权字体”，也不能单独
销售。任何包含这些字体的 SDK 或二进制分发都必须同时保留：

- AppKit 仓库的 `fonts/licenses/NOTICE.md`
- AppKit 仓库的 `fonts/licenses/OFL-1.1.txt`

## 设计验收清单

- 西文、数字和西文标点显示为 Inter；中文显示为思源黑体。
- 中西文混排使用同一 AppKit 复合字体或以其为 fallback 的增量字体，基线和字号一致。
- 界面只使用规定的五档字号及对应公共 token。
- 页面使用 Flex/Grid 等响应式布局技术，不依赖单一分辨率或大量绝对坐标。
- 最小、典型和最大目标视口以及横竖屏下均无重叠、裁切或不可达操作。
- 容器尺寸或方向动态变化后，布局正确重排且业务状态、焦点和用户输入保持不变。
- 新增中文字符已进入合适的 AppKit 或应用私有裁剪字符集，并已验证 fallback。
- 字体在目标分辨率、横竖屏及明暗主题下没有溢出、遮挡或缺字。
- 字体授权文件随所有嵌入字体的产物一起分发。
