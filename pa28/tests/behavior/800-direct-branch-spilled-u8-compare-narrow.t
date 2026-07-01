function @probe(%a : ptr [pass=reference], %b : ptr [pass=reference], %c : ptr [pass=reference], %d : ptr [pass=reference]) -> u8 {
  block ^entry:
    return u8 0
}

function @clobber() -> void {
  block ^entry:
    return void
}

function @main() -> i64 [role=entry] {
  slot $value : obj<24x8>
  slot $ud_suffix : obj<24x8>
  slot $literal_type : i32
  slot $call__1 : u8
  slot $k0 : i64
  slot $k1 : i64
  slot $k2 : i64
  slot $k3 : i64
  slot $k4 : i64

  block ^entry:
    store i32 1, $literal_type
    store i64 10, $k0
    store i64 11, $k1
    store i64 12, $k2
    store i64 13, $k3
    store i64 14, $k4
    %t1 = addr $value
    %t2 = addr $ud_suffix
    %t3 = addr $literal_type
    %t4 = addr $ud_suffix
    %t5 = call u8 @probe(%t1, %t2, %t3, %t4)
    store u8 %t5, $call__1
    %a0 = load i64 $k0
    %a1 = load i64 $k1
    %a2 = load i64 $k2
    %a3 = load i64 $k3
    %a4 = load i64 $k4
    %t6 = load u8 $call__1
    call void @clobber()
    %t7 = cmp eq i64 %t6, 0
    branch %t7, ^then, ^else

  block ^then:
    %s0 = binary add i64 %a0, %a1
    %s1 = binary add i64 %a2, %a3
    %s2 = binary add i64 %s0, %s1
    %s3 = binary add i64 %s2, %a4
    return i64 %s3

  block ^else:
    return i64 0
}
