function @sink(%a : i64, %b : i64, %c : i64, %d : i64, %e : i64, %f : i64, %created : ptr) -> void {
  block ^entry:
    store u8 1, %created
    return void
}

function @main() -> i32 [role=entry] {
  slot $created : u8

  block ^entry:
    store u8 0, $created
    %t1 = addr $created
    call void @sink(1, 2, 3, 4, 5, 6, %t1)
    %t2 = load u8 $created
    %t3 = cmp ne i64 %t2, 1
    return i32 %t3
}
