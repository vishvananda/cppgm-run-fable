global @values = {
  i64 11
  i64 22
  i64 33
}
global @holder = {
  ptr addr @values
}

function @g(%this : ptr, %__i : i64) -> ptr {
  slot $this : ptr
  slot $__i : i64

  block ^entry:
    store ptr %this, $this
    store i64 %__i, $__i
    %t1 = load ptr $this
    %t2 = index i8 [projection=field] %t1, 0
    %t3 = load ptr %t2
    %t4 = load i64 $__i
    %t5 = index ptr [projection=array_element] %t3, %t4
    return ptr %t5
}

function @main() -> i64 [role=entry] {
  block ^entry:
    %this = addr @holder
    %p = call ptr @g(%this, 2)
    %v = load i64 %p
    %bad = cmp ne i64 %v, 33
    return i64 %bad
}
