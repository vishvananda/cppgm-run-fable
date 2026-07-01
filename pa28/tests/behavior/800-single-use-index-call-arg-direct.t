global @lhs = {
  i64 10
  i64 20
}
global @rhs = {
  i64 1
  i64 2
  i64 3
}

function @g(%a : ptr, %b : ptr) -> i64 {
  block ^entry:
    %av = load i64 %a
    %bv = load i64 %b
    %sum = binary add i64 %av, %bv
    return i64 %sum
}

function @f(%this : ptr, %rhs : ptr) -> i64 {
  block ^entry:
    %dst = index i64 %this, 1
    %src = index i64 %rhs, 2
    %r = call i64 @g(%dst, %src)
    return i64 %r
}

function @main() -> i64 [role=entry] {
  block ^entry:
    %a = addr @lhs
    %b = addr @rhs
    %r = call i64 @f(%a, %b)
    %bad = cmp ne i64 %r, 23
    return i64 %bad
}
