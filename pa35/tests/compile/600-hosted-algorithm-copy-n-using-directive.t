#include <algorithm>

using namespace std;

int src[2] = {1, 2};
int dst[2];

void copy_n_anchor()
{
  copy_n(src, 2, dst);
}
static_assert(sizeof(&copy_n_anchor) > 0, "copy_n body anchor");
