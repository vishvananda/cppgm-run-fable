global @cell = {
  i64 11
}

function @g(%a : ptr, %b : i64) -> ptr {
  block ^entry:
    store i64 %b, %a
    return ptr %a
}

function @f(%this : ptr, %hash : i64) -> ptr {
  slot $this_slot : ptr
  slot $hash_slot : i64

  block ^entry:
    store ptr %this, $this_slot
    store i64 %hash, $hash_slot
    %base = load ptr $this_slot
    %dst = index i8 [projection=field] %base, 0
    %value = load i64 $hash_slot
    %result = call ptr @g(%dst, %value)
    return ptr %result
}

function @main() -> i64 [role=entry] {
  block ^entry:
    %base = addr @cell
    %r = call ptr @f(%base, 123)
    %wrong_ptr = cmp ne ptr %r, %base
    branch %wrong_ptr, ^bad_ptr, ^check_value

  block ^bad_ptr:
    return i64 1

  block ^check_value:
    %v = load i64 %base
    %wrong_value = cmp ne i64 %v, 123
    return i64 %wrong_value
}
