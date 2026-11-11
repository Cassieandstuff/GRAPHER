#pragma once
/// @file
/// @brief The look of a canvas that is not a node or edge type's own: background, grid, card chrome, selection. Sizes
///        are in world units at zoom 1.

#include <grapher/Geometry.h>

#include <cstdint>

namespace grapher {

struct Style {
    Color background{24, 25, 28};
    Color grid{34, 36, 40};
    Color gridMajor{44, 46, 52};
    Color nodeFill{40, 42, 48};
    Color noteFill{52, 50, 40};
    Color nodeStroke{66, 69, 76};
    Color selection{92, 160, 255};
    Color text{226, 228, 233};
    Color muted{150, 154, 162};
    Color marquee{92, 160, 255, 36};

    float gridSpacing = 20.f;
    int   majorEvery  = 5;
    float fontSize    = 12.f;
    float titleSize   = 15.f;
    float corner      = 6.f;
    float band        = 4.f;    ///< the accent strip's width
    float padding     = 10.f;
    float handle      = 5.f;    ///< the connect handle's radius
    float resizeGrip  = 12.f;
    float minNode     = 80.f;   ///< the smallest a node may be resized to
    float detailZoom  = 0.45f;  ///< below this zoom a card shows only its title
    float textZoom    = 0.3f;   ///< below this zoom no text draws at all, only shapes
    float groupHeader = 30.f;   ///< a group's header strip: its title sits there, and a click there picks the group
    float groupPadding = 14.f;  ///< between a member group's outline and what it holds
    float dockGap      = 10.f;  ///< between docked nodes stacked above or below their parent
    float dockSideGap  = 12.f;  ///< between a parent and a node docked on its left or right
    Color glow{255, 196, 64};    ///< Highlights' glow and arrows
    Color searchRing{255, 140, 60};   ///< the other search hits
    bool  highlightSelectedWires = false;  ///< wires touching a selected node draw in the selection colour
    std::uint8_t groupFill = 22;   ///< the alpha of a group's band-coloured fill
    friend bool operator==(const Style&, const Style&) = default;
};

} // namespace grapher
