// VALIDATION: compile-pass
// N3485 focus: 14.3.1 [temp.arg.type], 7.1.3 [dcl.typedef]

typedef unsigned int mask;

struct node {
  int value;
};

typedef node node_alias;

template<class T>
struct box {
};

box<mask> *pmask;
box<node_alias> *pnode;

int main()
{
  return pmask == 0 && pnode == 0 ? 0 : 1;
}
