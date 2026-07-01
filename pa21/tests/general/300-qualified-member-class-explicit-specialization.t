namespace fusion
{
namespace extension
{
struct access
{
  template<class Seq, int N>
  struct struct_member;
};
}
}

namespace test_detail
{
struct adapted_sequence
{
  int data;
};
}

namespace fusion
{
namespace extension
{
template<>
struct access::struct_member<test_detail::adapted_sequence, 0>
{
  typedef int type;
};
}
}

typedef typename fusion::extension::access::struct_member<
    test_detail::adapted_sequence, 0>::type member_type;

int main()
{
  member_type value = 0;
  return value;
}
