#pragma once
/// @file
/// @brief Ports: the named, typed places on a node a wire connects to. A node type declares its ports in the schema; a
///        node may add its own (a list whose length is data, a block with a variable number of inputs). A wire runs from
///        an Out port to an In port, and whether it may is the schema's to say (Schema::CanConnect): by port type, by
///        the types' `bases`, and by the conversions the schema lists.

#include <grapher/Geometry.h>

#include <string>
#include <vector>

namespace grapher {

/// @brief Where a node's picture slot (Node::image) sits. Here because both the Schema (a type's) and the Graph (a
///        node's own) name it.
enum class ImagePlacement {
    Foot,    ///< inside the card, at its foot: the card has room for it (CardHeight counts it)
    Below,   ///< hanging under the card's frame: not part of the card (not counted, not an obstacle), drawn under
             ///< every card and wire, so a neighbour overlapping it covers it
};

enum class PortDir {
    In,    ///< a wire ends here
    Out,   ///< a wire starts here
};

enum class PortSide {
    Auto,   ///< In on the left, Out on the right
    Left,
    Right,
    Top,
    Bottom,
};

/// @brief How a port's pin is drawn. A pin is filled while a wire uses it and hollow while none does.
enum class PortGlyph {
    Circle,
    Square,
    Diamond,
    Arrow,   ///< a triangle pointing the way the wire runs
};

/// @brief What a port carries: a value type (a float, a colour, an SDF), a class a reference may point at.
struct PortType {
    std::string              id;      ///< PortDef::type refers to it, byte-exact
    std::string              label;
    Color                    color{150, 154, 162};
    PortGlyph                glyph = PortGlyph::Circle;
    std::vector<std::string> bases;   ///< the types this one is (is-a), each a PortType id; see Schema::subtypeFlow
    std::string              edge;    ///< an EdgeType id a wire from a port of this type gets; empty = Schema::EdgeFor
    /// @brief The kit's pin kind its pins draw as (a role many types share); empty = the kind its id finds.
    std::string              pin;
    friend bool operator==(const PortType&, const PortType&) = default;
};

struct PortDef {
    std::string id;      ///< unique on its node, byte-exact; Edge::fromPort / toPort refer to it
    std::string label;   ///< what a person reads on the port's row; empty = no row text
    std::string type;    ///< a PortType id; empty connects to anything
    PortDir     dir  = PortDir::In;
    PortSide    side = PortSide::Auto;
    /// @brief How many wires the port takes; a new wire past it replaces the oldest. -1 = by direction: an In port
    ///        takes one, an Out port any number. 0 = any number.
    int         maxWires = -1;
    std::string field;   ///< a FieldDef id whose value the row shows while no wire uses the port (a default, a literal)
    bool        onHeader = false;   ///< sits on the title line instead of a row of its own
    /// @brief Another port of the same node whose wired type this one takes (a reroute's output carries whatever comes
    ///        into its input); `type` applies while that port has no wire. See PortTypeOf.
    std::string typeFrom;
    std::string tooltip;   ///< shown over the pin; empty = its label and type
    friend bool operator==(const PortDef&, const PortDef&) = default;
};

/// @brief A wire from an Out port of type `from` may end on an In port of type `to` (an implicit conversion).
struct Conversion {
    std::string from;
    std::string to;
    friend bool operator==(const Conversion&, const Conversion&) = default;
};

/// @brief Which end of a wire may be the more specific type.
enum class SubtypeFlow {
    OutIsA,   ///< data graphs: a value flows into a port of its own type or a base of it (a Float into a Number input)
    InIsA,    ///< reference graphs: a pointer of class C takes a node of class C or one deriving from it
};

/// @return how many wires the port takes; 0 = any number
[[nodiscard]] inline int MaxWires(const PortDef& p) { return p.maxWires >= 0 ? p.maxWires : (p.dir == PortDir::In ? 1 : 0); }

/// @return the side the port sits on, Auto resolved
[[nodiscard]] inline PortSide SideOf(const PortDef& p)
{
    if (p.side != PortSide::Auto) return p.side;
    return p.dir == PortDir::In ? PortSide::Left : PortSide::Right;
}

} // namespace grapher
