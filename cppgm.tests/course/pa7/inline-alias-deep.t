namespace I0 {
  inline namespace I1 { typedef int TI; extern TI v; namespace Deep { typedef char DC; } }
}
I0::TI a;
namespace AL = I0::I1::Deep;
AL::DC c;
I0::Deep::DC d;
using namespace I0;
TI e;
