// 5.2.5p4: a non-reference member of a non-lvalue object keeps the
// object's expiring-ness - E1.E2 is an xvalue when E1 is an xvalue -
// while the same member of an lvalue object stays an lvalue.
struct N
{
	int v;
};

struct V
{
	N n;
};

int use_xvalue(V& obj)
{
	return static_cast<V&&>(obj).n.v;
}

int use_lvalue(V& obj)
{
	return obj.n.v;
}
