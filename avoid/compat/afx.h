#pragma once
// libavoid's debug assertions include MFC's <afx.h> on MSVC just for ASSERT. This stands in for it (it is found first on
// the avoid include path), so a Debug build needs no MFC and libavoid keeps its assertions.
#include <cassert>
#ifndef ASSERT
#define ASSERT(expr) assert(expr)
#endif
