namespace U { typedef short TS; extern long lv; }
using U::TS;
TS s;
namespace W { using U::lv; }
namespace W2 { using namespace W; }
