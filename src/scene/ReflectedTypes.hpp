#pragma once

namespace saida {

// Central registration avoids static-library dead stripping of per-TU registrars.
void registerReflectedTypes();

} // namespace saida
