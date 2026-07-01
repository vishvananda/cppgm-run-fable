global @expected_template_arguments : ptr [binding=internal, object=_Z27expected_template_arguments] = 0

function @function_binding_qualified_name_for_symbol(%binding : ptr [pass=reference]) -> i32 [binding=internal, object=_Z42function_binding_qualified_name_for_symbolRK15FunctionBinding] {
  slot $binding : ptr

  block ^entry:
    store ptr %binding, $binding
    %t1 = load ptr $binding
    %t2 = index i8 [projection=field] %t1, 160
    %t3 = load i32 %t2
    return i32 %t3
}
function @function_binding_display_name_for_symbol(%binding : ptr [pass=reference]) -> i32 [binding=internal, object=_Z40function_binding_display_name_for_symbolRK15FunctionBinding] {
  slot $binding : ptr

  block ^entry:
    store ptr %binding, $binding
    %t1 = load ptr $binding
    %t2 = index i8 [projection=field] %t1, 164
    %t3 = load i32 %t2
    return i32 %t3
}
function @function_object_symbol_key(%binding : ptr [pass=reference]) -> i32 [binding=internal, object=_Z26function_object_symbol_keyRK15FunctionBinding] {
  slot $binding : ptr

  block ^entry:
    store ptr %binding, $binding
    %t1 = load ptr $binding
    %t2 = index i8 [projection=field] %t1, 376
    %t3 = load i32 %t2
    return i32 %t3
}
function @make_function_symbol_identity(%ret : ptr [pass=indirect_result], %qualified_name : i32, %display_name : i32, %__param2 : u8, %__param3 : ptr, %options : ptr [pass=reference], %key : i32, %linkage : i32) -> void [binding=internal, object=_Z29make_function_symbol_identityiibPKiRK7Optionsi13SymbolLinkage] {
  slot $qualified_name : i32
  slot $display_name : i32
  slot $__param2 : u8
  slot $__param3 : ptr
  slot $options : ptr
  slot $key : i32
  slot $linkage : i32
  slot $cond__1 : i32

  block ^entry:
    store i32 %qualified_name, $qualified_name
    store i32 %display_name, $display_name
    store u8 %__param2, $__param2
    store ptr %__param3, $__param3
    store ptr %options, $options
    store i32 %key, $key
    store i32 %linkage, $linkage
    call void @SymbolIdentity__SymbolIdentity(%ret)
    %t1 = load i32 $qualified_name
    %t2 = index i8 [projection=field] %ret, 0
    %t3 = unary decay ptr %t2
    %t4 = index i64 [projection=array_element] %t3, 0
    store i64 %t1, %t4
    %t5 = load i32 $display_name
    %t6 = index i8 [projection=field] %ret, 0
    %t7 = unary decay ptr %t6
    %t8 = index i64 [projection=array_element] %t7, 1
    store i64 %t5, %t8
    %t9 = load i32 $key
    %t10 = index i8 [projection=field] %ret, 0
    %t11 = unary decay ptr %t10
    %t12 = index i64 [projection=array_element] %t11, 2
    store i64 %t9, %t12
    %t13 = load ptr $options
    %t14 = index i8 [projection=field] %t13, 24
    %t15 = load ptr %t14
    %t16 = index i8 [projection=field] %ret, 32
    store ptr %t15, %t16
    %t17 = load ptr $options
    %t18 = index i8 [projection=field] %t17, 24
    %t19 = load ptr %t18
    %t20 = load ptr @expected_template_arguments
    %t21 = cmp eq ptr %t19, %t20
    branch %t21, ^cond_then_1, ^cond_else_2

  block ^cond_then_1:
    store i32 0, $cond__1
    jump ^cond_end_3

  block ^cond_else_2:
    store i32 10, $cond__1
    jump ^cond_end_3

  block ^cond_end_3:
    %t22 = load i32 $cond__1
    %t23 = index i8 [projection=field] %ret, 40
    store i32 %t22, %t23
    %t24 = load i32 $linkage
    %t25 = index i8 [projection=field] %ret, 48
    store i32 %t24, %t25
    return void
}
function @main() -> i32 [role=entry, binding=strong, keep_alias=yes] {
  slot $type_value : i32
  slot $binding : obj<384x8>
  slot $analyzer : obj<1x1>
  slot $out : obj<56x8>

  block ^entry:
    store i32 5, $type_value
    %t1 = addr $binding
    call void @FunctionBinding__FunctionBinding(%t1)
    %t2 = addr $binding
    %t3 = index i8 [projection=field] %t2, 160
    store i32 101, %t3
    %t4 = addr $binding
    %t5 = index i8 [projection=field] %t4, 164
    store i32 202, %t5
    %t6 = addr $type_value
    %t7 = addr $binding
    %t8 = index i8 [projection=field] %t7, 168
    store ptr %t6, %t8
    %t9 = addr $binding
    %t10 = index i8 [projection=field] %t9, 176
    store u8 1, %t10
    %t11 = addr $binding
    %t12 = index i8 [projection=field] %t11, 180
    store i32 17, %t12
    %t13 = addr $binding
    %t14 = index i8 [projection=field] %t13, 184
    store i32 3, %t14
    %t15 = addr $binding
    %t16 = index i8 [projection=field] %t15, 188
    store u8 1, %t16
    %t17 = copy ptr 4660
    %t18 = addr $binding
    %t19 = index i8 [projection=field] %t18, 192
    store ptr %t17, %t19
    %t20 = addr $binding
    %t21 = index i8 [projection=field] %t20, 264
    %t22 = index i8 [projection=field] %t21, 48
    store i32 1, %t22
    %t23 = addr $binding
    %t24 = index i8 [projection=field] %t23, 264
    %t25 = index i8 [projection=field] %t24, 44
    store u8 1, %t25
    %t26 = addr $binding
    %t27 = index i8 [projection=field] %t26, 264
    %t28 = index i8 [projection=field] %t27, 45
    store u8 1, %t28
    %t29 = addr $binding
    %t30 = index i8 [projection=field] %t29, 320
    %t31 = index i8 [projection=field] %t30, 0
    %t32 = unary decay ptr %t31
    %t33 = index i32 [projection=array_element] %t32, 0
    store i32 7, %t33
    %t34 = addr $binding
    %t35 = index i8 [projection=field] %t34, 336
    store u8 1, %t35
    %t36 = copy ptr 17767
    %t37 = addr $binding
    %t38 = index i8 [projection=field] %t37, 344
    store ptr %t36, %t38
    %t39 = copy ptr 35243
    %t40 = addr $binding
    %t41 = index i8 [projection=field] %t40, 352
    store ptr %t39, %t41
    %t42 = copy ptr 52719
    %t43 = addr $binding
    %t44 = index i8 [projection=field] %t43, 360
    store ptr %t42, %t44
    %t45 = copy ptr 4369
    %t46 = addr $binding
    %t47 = index i8 [projection=field] %t46, 368
    store ptr %t45, %t47
    %t48 = addr $binding
    %t49 = index i8 [projection=field] %t48, 376
    store i32 303, %t49
    %t50 = addr $binding
    %t51 = index i8 [projection=field] %t50, 320
    store ptr %t51, @expected_template_arguments
    %t52 = addr $analyzer
    %t53 = addr $out
    %t54 = addr $analyzer
    %t55 = addr $binding
    call void @Analyzer__direct_call_symbol(%t53, %t54, %t55)
    %t56 = addr $out
    %t57 = index i8 [projection=field] %t56, 40
    %t58 = load i32 %t57
    %t59 = cmp ne i32 %t58, 0
    branch %t59, ^if_then_1, ^if_else_2

  block ^if_then_1:
    %t60 = addr $out
    %t61 = index i8 [projection=field] %t60, 40
    %t62 = load i32 %t61
    return i32 %t62

  block ^if_else_2:
    jump ^if_end_3

  block ^if_end_3:
    %t63 = addr $out
    %t64 = index i8 [projection=field] %t63, 32
    %t65 = load ptr %t64
    %t66 = addr $binding
    %t67 = index i8 [projection=field] %t66, 320
    %t68 = cmp ne ptr %t65, %t67
    branch %t68, ^if_then_4, ^if_else_5

  block ^if_then_4:
    return i32 20

  block ^if_else_5:
    jump ^if_end_6

  block ^if_end_6:
    %t69 = addr $out
    %t70 = index i8 [projection=field] %t69, 44
    %t71 = load u8 %t70
    %t72 = cmp eq i64 %t71, 0
    branch %t72, ^if_then_7, ^lor_rhs_10

  block ^lor_rhs_10:
    %t73 = addr $out
    %t74 = index i8 [projection=field] %t73, 45
    %t75 = load u8 %t74
    %t76 = cmp eq i64 %t75, 0
    branch %t76, ^if_then_7, ^if_else_8

  block ^if_then_7:
    return i32 30

  block ^if_else_8:
    jump ^if_end_9

  block ^if_end_9:
    return i32 0
}
function @SymbolIdentity__SymbolIdentity(%this : ptr) -> void [unwind=no, binding=weak, object=_ZN14SymbolIdentityC1Ev] {
  slot $this : ptr

  block ^entry:
    store ptr %this, $this
    %t1 = load ptr $this
    %t2 = index i8 [projection=field] %t1, 32
    store ptr 0, %t2
    %t3 = load ptr $this
    %t4 = index i8 [projection=field] %t3, 40
    store i32 0, %t4
    %t5 = load ptr $this
    %t6 = index i8 [projection=field] %t5, 44
    store u8 0, %t6
    %t7 = load ptr $this
    %t8 = index i8 [projection=field] %t7, 45
    store u8 0, %t8
    %t9 = load ptr $this
    %t10 = index i8 [projection=field] %t9, 48
    store i32 1, %t10
    return void
}
function @SymbolIdentity__SymbolIdentity__ov2(%this : ptr, %other : ptr [pass=reference]) -> void [unwind=no, binding=weak, object=_ZN14SymbolIdentityC1EOS_] {
  slot $this : ptr
  slot $other : ptr

  block ^entry:
    store ptr %this, $this
    store ptr %other, $other
    %t1 = load ptr $this
    %t2 = load ptr $other
    copyobj 52x8 %t2, %t1
    return void
}
function @FunctionBinding__FunctionBinding(%this : ptr) -> void [unwind=no, binding=weak, object=_ZN15FunctionBindingC1Ev] {
  slot $this : ptr

  block ^entry:
    store ptr %this, $this
    %t1 = load ptr $this
    %t2 = index i8 [projection=field] %t1, 160
    store i32 0, %t2
    %t3 = load ptr $this
    %t4 = index i8 [projection=field] %t3, 164
    store i32 0, %t4
    %t5 = load ptr $this
    %t6 = index i8 [projection=field] %t5, 168
    store ptr 0, %t6
    %t7 = load ptr $this
    %t8 = index i8 [projection=field] %t7, 176
    store u8 0, %t8
    %t9 = load ptr $this
    %t10 = index i8 [projection=field] %t9, 177
    store u8 0, %t10
    %t11 = load ptr $this
    %t12 = index i8 [projection=field] %t11, 180
    store i32 0, %t12
    %t13 = load ptr $this
    %t14 = index i8 [projection=field] %t13, 184
    store i32 0, %t14
    %t15 = load ptr $this
    %t16 = index i8 [projection=field] %t15, 188
    store u8 0, %t16
    %t17 = load ptr $this
    %t18 = index i8 [projection=field] %t17, 189
    store u8 0, %t18
    %t19 = load ptr $this
    %t20 = index i8 [projection=field] %t19, 190
    store u8 0, %t20
    %t21 = load ptr $this
    %t22 = index i8 [projection=field] %t21, 192
    store ptr 0, %t22
    %t23 = load ptr $this
    %t24 = index i8 [projection=field] %t23, 264
    call void @SymbolIdentity__SymbolIdentity(%t24)
    %t25 = load ptr $this
    %t26 = index i8 [projection=field] %t25, 336
    store u8 0, %t26
    %t27 = load ptr $this
    %t28 = index i8 [projection=field] %t27, 344
    store ptr 0, %t28
    %t29 = load ptr $this
    %t30 = index i8 [projection=field] %t29, 352
    store ptr 0, %t30
    %t31 = load ptr $this
    %t32 = index i8 [projection=field] %t31, 360
    store ptr 0, %t32
    %t33 = load ptr $this
    %t34 = index i8 [projection=field] %t33, 368
    store ptr 0, %t34
    %t35 = load ptr $this
    %t36 = index i8 [projection=field] %t35, 376
    store i32 0, %t36
    return void
}
function @Analyzer__Analyzer(%this : ptr) -> void [unwind=no, binding=weak, object=_ZN8AnalyzerC1Ev] {
  slot $this : ptr

  block ^entry:
    store ptr %this, $this
    return void
}
function @Analyzer__direct_call_symbol(%ret : ptr [pass=indirect_result], %this : ptr, %target : ptr [pass=reference]) -> void [binding=weak, object=_ZN8Analyzer18direct_call_symbolERK15FunctionBinding] {
  slot $this : ptr
  slot $target : ptr
  slot $options : obj<72x8>
  slot $updated : obj<56x8>

  block ^entry:
    store ptr %this, $this
    store ptr %target, $target
    %t1 = load ptr $target
    %t2 = index i8 [projection=field] %t1, 188
    %t3 = load u8 %t2
    %t4 = cmp eq i64 %t3, 0
    branch %t4, ^land_rhs_4, ^if_else_2

  block ^land_rhs_4:
    %t5 = load ptr $target
    %t6 = index i8 [projection=field] %t5, 189
    %t7 = load u8 %t6
    %t8 = cmp eq i64 %t7, 0
    branch %t8, ^if_then_1, ^if_else_2

  block ^if_then_1:
    %t9 = load ptr $target
    %t10 = index i8 [projection=field] %t9, 264
    call void @SymbolIdentity__SymbolIdentity__ov3(%ret, %t10)
    return void

  block ^if_else_2:
    jump ^if_end_3

  block ^if_end_3:
    %t11 = addr $options
    call void @Options__Options(%t11)
    %t12 = load ptr $this
    %t13 = addr $options
    %t14 = load ptr $target
    %t15 = index i8 [projection=field] %t14, 176
    %t16 = load u8 %t15
    %t17 = load ptr $target
    %t18 = index i8 [projection=field] %t17, 177
    %t19 = load u8 %t18
    %t20 = load ptr $target
    %t21 = index i8 [projection=field] %t20, 180
    %t22 = load i32 %t21
    %t23 = load ptr $target
    %t24 = index i8 [projection=field] %t23, 188
    %t25 = load u8 %t24
    %t26 = load ptr $target
    %t27 = index i8 [projection=field] %t26, 189
    %t28 = load u8 %t27
    %t29 = load ptr $target
    %t30 = index i8 [projection=field] %t29, 192
    %t31 = load ptr %t30
    %t32 = load ptr $target
    %t33 = index i8 [projection=field] %t32, 320
    %t34 = load ptr $target
    %t35 = index i8 [projection=field] %t34, 336
    %t36 = load u8 %t35
    %t37 = load ptr $target
    %t38 = index i8 [projection=field] %t37, 344
    %t39 = load ptr %t38
    %t40 = load ptr $target
    %t41 = index i8 [projection=field] %t40, 352
    %t42 = load ptr %t41
    %t43 = load ptr $target
    %t44 = index i8 [projection=field] %t43, 360
    %t45 = load ptr %t44
    %t46 = load ptr $target
    %t47 = index i8 [projection=field] %t46, 368
    %t48 = load ptr %t47
    call void @Analyzer__populate(%t12, %t13, %t16, %t19, %t22, %t25, %t28, %t31, %t33, %t36, %t39, %t42, %t45, %t48)
    %t49 = load ptr $target
    %t50 = index i8 [projection=field] %t49, 184
    %t51 = load i32 %t50
    %t52 = addr $options
    %t53 = index i8 [projection=field] %t52, 8
    store i32 %t51, %t53
    %t54 = addr $updated
    %t55 = load ptr $target
    %t56 = call i32 @function_binding_qualified_name_for_symbol(%t55)
    %t57 = load ptr $target
    %t58 = call i32 @function_binding_display_name_for_symbol(%t57)
    %t59 = load ptr $target
    %t60 = index i8 [projection=field] %t59, 190
    %t61 = load u8 %t60
    %t62 = load ptr $target
    %t63 = index i8 [projection=field] %t62, 168
    %t64 = load ptr %t63
    %t65 = addr $options
    %t66 = load ptr $target
    %t67 = call i32 @function_object_symbol_key(%t66)
    %t68 = load ptr $target
    %t69 = index i8 [projection=field] %t68, 264
    %t70 = index i8 [projection=field] %t69, 48
    %t71 = load i32 %t70
    call void @make_function_symbol_identity(%t54, %t56, %t58, %t61, %t64, %t65, %t67, %t71)
    %t72 = load ptr $target
    %t73 = index i8 [projection=field] %t72, 264
    %t74 = index i8 [projection=field] %t73, 44
    %t75 = load u8 %t74
    %t76 = addr $updated
    %t77 = index i8 [projection=field] %t76, 44
    store u8 %t75, %t77
    %t78 = load ptr $target
    %t79 = index i8 [projection=field] %t78, 264
    %t80 = index i8 [projection=field] %t79, 45
    %t81 = load u8 %t80
    %t82 = addr $updated
    %t83 = index i8 [projection=field] %t82, 45
    store u8 %t81, %t83
    %t84 = addr $updated
    call void @SymbolIdentity__SymbolIdentity__ov2(%ret, %t84)
    return void
}
function @Analyzer__populate(%this : ptr, %options : ptr [pass=reference], %is_const_method : u8, %is_volatile_method : u8, %ref_qualifier : i32, %is_constructor : u8, %is_destructor : u8, %source_template : ptr, %instantiation_arguments : ptr, %has_instantiation_arguments : u8, %owner_class : ptr, %declaration_scope : ptr, %declaration_node : ptr, %definition_node : ptr) -> void [binding=weak, object=_ZN8Analyzer8populateER7OptionsbbibbPvPK17TemplateArgumentsbPKvS7_S7_S7_] {
  slot $this : ptr
  slot $options : ptr
  slot $is_const_method : u8
  slot $is_volatile_method : u8
  slot $ref_qualifier : i32
  slot $is_constructor : u8
  slot $is_destructor : u8
  slot $source_template : ptr
  slot $instantiation_arguments : ptr
  slot $has_instantiation_arguments : u8
  slot $owner_class : ptr
  slot $declaration_scope : ptr
  slot $declaration_node : ptr
  slot $definition_node : ptr

  block ^entry:
    store ptr %this, $this
    store ptr %options, $options
    store u8 %is_const_method, $is_const_method
    store u8 %is_volatile_method, $is_volatile_method
    store i32 %ref_qualifier, $ref_qualifier
    store u8 %is_constructor, $is_constructor
    store u8 %is_destructor, $is_destructor
    store ptr %source_template, $source_template
    store ptr %instantiation_arguments, $instantiation_arguments
    store u8 %has_instantiation_arguments, $has_instantiation_arguments
    store ptr %owner_class, $owner_class
    store ptr %declaration_scope, $declaration_scope
    store ptr %declaration_node, $declaration_node
    store ptr %definition_node, $definition_node
    %t1 = load ptr $owner_class
    %t2 = cmp ne ptr %t1, 0
    %t3 = load ptr $options
    %t4 = index i8 [projection=field] %t3, 0
    store u8 %t2, %t4
    %t5 = load u8 $is_const_method
    %t6 = load ptr $options
    %t7 = index i8 [projection=field] %t6, 1
    store u8 %t5, %t7
    %t8 = load u8 $is_volatile_method
    %t9 = load ptr $options
    %t10 = index i8 [projection=field] %t9, 2
    store u8 %t8, %t10
    %t11 = load i32 $ref_qualifier
    %t12 = load ptr $options
    %t13 = index i8 [projection=field] %t12, 4
    store i32 %t11, %t13
    %t14 = load u8 $is_constructor
    %t15 = load ptr $options
    %t16 = index i8 [projection=field] %t15, 12
    store u8 %t14, %t16
    %t17 = load u8 $is_destructor
    %t18 = load ptr $options
    %t19 = index i8 [projection=field] %t18, 13
    store u8 %t17, %t19
    %t20 = load ptr $source_template
    %t21 = cmp ne ptr %t20, 0
    branch %t21, ^land_rhs_5, ^if_else_2

  block ^land_rhs_5:
    %t22 = load ptr $instantiation_arguments
    %t23 = cmp ne ptr %t22, 0
    branch %t23, ^land_rhs_4, ^if_else_2

  block ^land_rhs_4:
    %t24 = load u8 $has_instantiation_arguments
    branch %t24, ^if_then_1, ^if_else_2

  block ^if_then_1:
    %t25 = load ptr $source_template
    %t26 = load ptr $options
    %t27 = index i8 [projection=field] %t26, 16
    store ptr %t25, %t27
    %t28 = load ptr $instantiation_arguments
    %t29 = load ptr $options
    %t30 = index i8 [projection=field] %t29, 24
    store ptr %t28, %t30
    %t31 = load ptr $options
    %t32 = index i8 [projection=field] %t31, 32
    store u8 1, %t32
    %t33 = load ptr $owner_class
    %t34 = load ptr $options
    %t35 = index i8 [projection=field] %t34, 40
    store ptr %t33, %t35
    %t36 = load ptr $declaration_scope
    %t37 = load ptr $options
    %t38 = index i8 [projection=field] %t37, 48
    store ptr %t36, %t38
    %t39 = load ptr $declaration_node
    %t40 = load ptr $options
    %t41 = index i8 [projection=field] %t40, 56
    store ptr %t39, %t41
    %t42 = load ptr $definition_node
    %t43 = load ptr $options
    %t44 = index i8 [projection=field] %t43, 64
    store ptr %t42, %t44
    jump ^if_end_3

  block ^if_else_2:
    jump ^if_end_3

  block ^if_end_3:
    return void
}
function @TemplateArguments__TemplateArguments(%this : ptr) -> void [unwind=no, binding=weak, object=_ZN17TemplateArgumentsC1Ev] {
  slot $this : ptr

  block ^entry:
    store ptr %this, $this
    return void
}
function @SymbolIdentity__SymbolIdentity__ov3(%this : ptr, %other : ptr [pass=reference]) -> void [unwind=no, binding=weak, object=_ZN14SymbolIdentityC1ERKS_] {
  slot $this : ptr
  slot $other : ptr

  block ^entry:
    store ptr %this, $this
    store ptr %other, $other
    %t1 = load ptr $this
    %t2 = load ptr $other
    copyobj 52x8 %t2, %t1
    return void
}
function @Options__Options(%this : ptr) -> void [unwind=no, binding=weak, object=_ZN7OptionsC1Ev] {
  slot $this : ptr

  block ^entry:
    store ptr %this, $this
    %t1 = load ptr $this
    %t2 = index i8 [projection=field] %t1, 0
    store u8 0, %t2
    %t3 = load ptr $this
    %t4 = index i8 [projection=field] %t3, 1
    store u8 0, %t4
    %t5 = load ptr $this
    %t6 = index i8 [projection=field] %t5, 2
    store u8 0, %t6
    %t7 = load ptr $this
    %t8 = index i8 [projection=field] %t7, 4
    store i32 0, %t8
    %t9 = load ptr $this
    %t10 = index i8 [projection=field] %t9, 8
    store i32 0, %t10
    %t11 = load ptr $this
    %t12 = index i8 [projection=field] %t11, 12
    store u8 0, %t12
    %t13 = load ptr $this
    %t14 = index i8 [projection=field] %t13, 13
    store u8 0, %t14
    %t15 = load ptr $this
    %t16 = index i8 [projection=field] %t15, 16
    store ptr 0, %t16
    %t17 = load ptr $this
    %t18 = index i8 [projection=field] %t17, 24
    store ptr 0, %t18
    %t19 = load ptr $this
    %t20 = index i8 [projection=field] %t19, 32
    store u8 0, %t20
    %t21 = load ptr $this
    %t22 = index i8 [projection=field] %t21, 40
    store ptr 0, %t22
    %t23 = load ptr $this
    %t24 = index i8 [projection=field] %t23, 48
    store ptr 0, %t24
    %t25 = load ptr $this
    %t26 = index i8 [projection=field] %t25, 56
    store ptr 0, %t26
    %t27 = load ptr $this
    %t28 = index i8 [projection=field] %t27, 64
    store ptr 0, %t28
    return void
}
