namespace A { typedef int TA; }
namespace B { using namespace A; }
namespace C { using namespace B; TA x1; }
namespace A { typedef char TA2; }
namespace C { TA2 x2; }
