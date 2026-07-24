#pragma once

// Light forward declarations for reflection, so behaviour/node *headers* can
// declare `static void describe(saida::reflect::TypeBuilder<T>&)` without pulling in
// the full reflection machinery (and nlohmann/json). The .cpp that defines
// describe() includes "core/Reflection.hpp".

namespace saida::reflect {
template <typename T>
class TypeBuilder;
}
