// VALIDATION: compile-pass

namespace dep_param_ref_array {
template <typename T>
T* begin(T&);
}

template <typename T>
char (&dep_param_ref_array_helper(T* t,
                                  decltype(dep_param_ref_array::begin(*t))*))[2];

int main()
{
  return sizeof(dep_param_ref_array_helper((int*)0, (int**)0)) == 2 ? 0 : 1;
}
