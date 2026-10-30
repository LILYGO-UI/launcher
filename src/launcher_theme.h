#ifndef LAUNCHER_THEME_H
#define LAUNCHER_THEME_H

#include <stdint.h>

namespace launcher::theme {

inline constexpr uint32_t kPageBackground      = 0xf2f2f7;
inline constexpr uint32_t kContainerBackground = 0xffffff;
inline constexpr uint32_t kContainerTitle      = 0x1a1a1a;
inline constexpr uint32_t kText                = 0x000000;
inline constexpr uint32_t kMutedText           = 0x6b6b70;
inline constexpr uint32_t kScrim               = 0x000000;
inline constexpr uint32_t kPanelShadow         = 0x000000;
inline constexpr uint32_t kTilePressedOverlay  = 0x000000;
inline constexpr uint32_t kControlBackground   = 0xe6e8ec;
inline constexpr uint32_t kControlPressed      = 0xd8dbe1;
inline constexpr uint32_t kControlTrack        = 0xc7c9cf;
inline constexpr uint32_t kControlIcon         = 0x48484a;
/* Keyboard focus follows the iOS system-blue accessibility treatment. */
inline constexpr uint32_t kControlFocus        = 0x007aff;
inline constexpr uint32_t kDestructive          = 0xff3b30;
inline constexpr uint32_t kControlAccent       = 0x19766f;
/* Account screens use the iOS system-blue family without changing the
 * launcher's existing teal accent used by quick controls and app tiles. */
inline constexpr uint32_t kAccountAccent       = 0x007aff;
inline constexpr uint32_t kAccountAccentPressed = 0x006fe6;
inline constexpr uint32_t kAccountFieldBackground = 0xf2f2f7;
inline constexpr uint32_t kAccountFieldBorder = 0xd1d1d6;
inline constexpr uint32_t kAccountPlaceholder  = 0x8e8e93;
inline constexpr uint32_t kAccountError        = 0xff3b30;
inline constexpr uint32_t kAccountKeyboard    = 0xe5e5ea;
inline constexpr uint32_t kIndicatorActive     = 0x18181a;
inline constexpr uint32_t kIndicatorInactive   = 0x8e8e93;

inline constexpr uint32_t kIconForeground    = 0xffffff;
inline constexpr uint32_t kIconSystem        = 0x596573;
inline constexpr uint32_t kIconPrimary       = 0x19766f;
inline constexpr uint32_t kIconMedia         = 0x9b5f3e;
inline constexpr uint32_t kIconConnectivity  = 0x416e93;
inline constexpr uint32_t kIconHome          = 0x8e6a3c;
inline constexpr uint32_t kIconUtility       = 0x277078;
inline constexpr uint32_t kIconCapture       = 0xb2534b;
inline constexpr uint32_t kIconCreative      = 0x7c4d9e;
inline constexpr uint32_t kIconPower         = 0x3d8b5f;
inline constexpr uint32_t kIconEnergy        = 0xd39128;
inline constexpr uint32_t kIconVideo         = 0x2f6f75;
inline constexpr uint32_t kIconCommunication = 0x31745a;
inline constexpr uint32_t kIconRadio         = 0x9a6a2f;
inline constexpr uint32_t kIconFallback      = 0x7b5269;

}  // namespace launcher::theme

#endif
