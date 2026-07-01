global @src = {
  i64 77
}
global @other = {
  i64 11
}

function @g(%a : ptr) -> ptr {
  block ^entry:
    return ptr %a
}

function @f(%a : ptr, %b : ptr, %c : ptr, %src : ptr) -> ptr {
  slot $tmp : obj<8x8>

  block ^entry:
    %keep = copy ptr %src
    %dst = addr $tmp
    copyobj 8x8 %src, %dst
    %r = call ptr @g(%keep)
    return ptr %r
}

function @main() -> i64 [role=entry] {
  block ^entry:
    %other = addr @other
    %src = addr @src
    %r = call ptr @f(%other, %other, %other, %src)
    %wrong_ptr = cmp ne ptr %r, %src
    branch %wrong_ptr, ^bad_ptr, ^check_value

  block ^bad_ptr:
    return i64 1

  block ^check_value:
    %v = load i64 %r
    %wrong_value = cmp ne i64 %v, 77
    return i64 %wrong_value
}
