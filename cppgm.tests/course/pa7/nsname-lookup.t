namespace N2 { typedef int T; extern int q; }
namespace M2 { int N2; using namespace ::N2; T t; }
namespace M3 { int N4; namespace N4b = N2; N4b::T u; }
namespace M4 { int N2; using namespace N2; T t4; }
