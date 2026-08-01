global @expected [binding=strong] = {
  i64 -3
}

function @dirty() -> void {
  slot $junk : obj<64x8>

  block ^entry:
    %base = addr $junk
    store i64 -1, %base
    %p1 = index i8 %base, 8
    store i64 -1, %p1
    %p2 = index i8 %base, 16
    store i64 -1, %p2
    %p3 = index i8 %base, 24
    store i64 -1, %p3
    %p4 = index i8 %base, 32
    store i64 -1, %p4
    %p5 = index i8 %base, 40
    store i64 -1, %p5
    %p6 = index i8 %base, 48
    store i64 -1, %p6
    %p7 = index i8 %base, 56
    store i64 -1, %p7
    return void
}

function @side() -> void {

  block ^entry:
    return void
}

function @fold(%a : i32, %b : i32, %c : i32, %d : i32, %e : i32, %f : i32) -> i32 {

  block ^entry:
    call void @side()
    %t1 = binary shr i32 %a, 1
    %t2 = binary div i32 %b, 2
    %t3 = binary mod i32 %c, 3
    %t4 = binary add i32 %t1, %t2
    %t5 = binary add i32 %t4, %t3
    %t6 = binary add i32 %t5, %d
    %t7 = binary add i32 %t6, %e
    %t8 = binary add i32 %t7, %f
    return i32 %t8
}

function @main() -> i32 [role=entry, binding=strong] {

  block ^entry:
    call void @dirty()
    %r = call i32 @fold(-8, -8, -7, 1, 2, 3)
    %ok = cmp eq i32 %r, -3
    branch %ok, ^good, ^bad

  block ^good:
    return i32 0

  block ^bad:
    return i32 1
}
