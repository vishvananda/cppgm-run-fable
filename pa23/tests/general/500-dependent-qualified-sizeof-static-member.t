// VALIDATION: compile-pass
// sizeof(type-id) recovery must still accept a dependent qualified static data
// member expression when the parser initially chose the type-id alternative.

namespace std
{
typedef unsigned long size_t;
}

template<bool Flag>
struct bucket_array_base
{
  static const std::size_t sizes[3];
  static const std::size_t sizes_length;
};

template<bool Flag>
const std::size_t bucket_array_base<Flag>::sizes[3] = {1, 2, 3};

template<bool Flag>
const std::size_t bucket_array_base<Flag>::sizes_length =
    sizeof(bucket_array_base<Flag>::sizes) /
    sizeof(bucket_array_base<Flag>::sizes[0]);

const std::size_t observed = bucket_array_base<true>::sizes_length;
