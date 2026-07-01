#include <cfloat>
#include <cmath>

#ifndef NAN
#error NAN should remain defined after cfloat and cmath
#endif

float nan_value()
{
  return NAN;
}
static_assert(sizeof(&nan_value) > 0, "NAN macro body anchor");
