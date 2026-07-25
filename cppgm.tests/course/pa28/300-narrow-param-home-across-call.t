global @table [binding=strong] = {
  i64 11
  i64 22
  i64 33
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

function @peek(%a : i32, %b : i32, %c : i32, %d : i32, %e : i32, %level : i32) -> i64 {

  block ^entry:
    call void @side()
    %t1 = addr @table
    %t2 = unary decay ptr %t1
    %t3 = binary mul i64 %level, 8
    %t4 = index i8 %t2, %t3
    %t5 = load i64 %t4
    %t6 = binary mul i64 %a, 1
    %t7 = binary mul i64 %b, 1
    %t8 = binary mul i64 %c, 1
    %t9 = binary mul i64 %d, 1
    %t10 = binary mul i64 %e, 1
    %t11 = binary add i64 %t5, %t6
    %t12 = binary add i64 %t11, %t7
    %t13 = binary add i64 %t12, %t8
    %t14 = binary add i64 %t13, %t9
    %t15 = binary add i64 %t14, %t10
    return i64 %t15
}

function @main() -> i32 [role=entry, binding=strong] {

  block ^entry:
    call void @dirty()
    %r = call i64 @peek(2, 3, 4, 5, 6, 1)
    %ok = cmp eq i64 %r, 42
    branch %ok, ^good, ^bad

  block ^good:
    return i32 0

  block ^bad:
    return i32 1
}
