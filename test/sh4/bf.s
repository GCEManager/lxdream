.section .text
.include "sh4/inc.s"
!
! Test bf pcrel
! Test bf/s pcrel

.global _test_bf
_test_bf:
	start_test
	
test_bf_1:   ! Test branch not taken
	add #1, r12
	setc
	bf test_bf_1_b
	bra test_bf_2
	nop
test_bf_1_b:
	fail test_bf_str_k
	
test_bf_2:	! Test branch taken
	add #1, r12
	clc
	bf test_bf_3
	fail test_bf_str_k

test_bf_3:	! Test branch taken (backwards)
	add #1, r12
	clc
	bra test_bf_3_b
	nop
	fail test_bf_str_k
	bra test_bf_4
test_bf_3_c:	
	nop
	bra test_bf_4
	nop
	fail test_bf_str_k
	bra test_bf_4
test_bf_3_b:
	nop
	bf test_bf_3_c
	fail test_bf_str_k
	bra test_bf_4	
	nop
test_bf_4:	! Test branch not taken w/ delay
	add #1, r12
	setc
	xor r0, r0
	bf/s test_bf_4_b
	add #1, r0
	bra test_bf_4_c
	nop
test_bf_4_b:
	fail test_bf_str_k
	bra test_bf_5
	nop
test_bf_4_c:
	xor r1,r1
	add #1, r1
	cmp/eq r0, r1

test_bf_5:	! Test branch taken w/ delay
	add #1, r12
	clc
	xor r0,r0
	bf/s test_bf_5_b
	add #1, r0
	fail test_bf_str_k
	bra test_bf_6
test_bf_5_b:	
	xor r1,r1
	add #1, r1
	cmp/eq r0,r1
	bt test_bf_6
	fail test_bf_str_k

test_bf_6:
	add #1, r12
	expect_exc 0x000001A0 ! BF is slot illegal
test_bf_6_exc:	
	bra test_bf_6_b
	bf test_bf_6_b
	assert_exc_caught test_bf_str_k test_bf_6_exc
	bra test_bf_end
	nop
test_bf_6_b:
test_bf_6_c:	
	fail test_bf_str_k
	
test_bf_end:
	end_test test_bf_str_k

test_bf_str:
	.string "BF"

.align 4	
test_bf_str_k:	
	.long test_bf_str
	