function @clobber(%a : i64, %b : i64, %c : i64, %d : i64, %e : i64, %f : i64) -> i64 {
  block ^entry:
    return i64 %e
}

function @f(%x : i64) -> i64 {
  slot $y : i64

  block ^entry:
    %t1 = copy i64 %x
    store i64 %t1, $y
    %ignored = call i64 @clobber(10, 20, 30, 40, 50, 60)
    %t2 = load i64 $y
    return i64 %t2
}

function @__cppgm_406d61696e() -> i32 [role=entry, binding=strong] {
  block ^entry:
    %x = const i64 41
    %y = call i64 @f(%x)
    %bad = cmp ne i64 %y, 41
    %ret = convert trunc i32 i64 %bad
    return i32 %ret
}
