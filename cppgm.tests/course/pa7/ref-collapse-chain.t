typedef int& LR;
typedef int&& RR;
using LRR = LR&&;
using RRL = RR&;
using RRR = RR&&;
extern LRR a;
extern RRL b;
extern RRR c;
void g(LR&&, RR&, RR&&);
