#include <stdio.h>

int total_tests = 0;
int total_fails = 0;

int test_print_result( char *testname, int failed, int total )
{
    fprintf( stdout, "%s: %d/%d tests passed\n", testname, total-failed, total );
    total_tests += total;
    total_fails += failed;
    return failed;
}

void test_print_failure( char *testname, int number, char *message )
{
    if( message == NULL ) {
	fprintf( stderr, "%s: Test %d failed!\n", testname, number );
    } else {
	fprintf( stderr, "%s: Test %d failed: %s\n", testname, number, message );
    }
}

extern unsigned int interrupt_pc;
extern unsigned int interrupt_count;

int assert_exception_caught( char *testname, int number, unsigned int expectedpc ) 
{
    if( interrupt_count == 0 ) {
	fprintf( stderr, "%s: Test %d failed: Expected exception not delivered\n",
		 testname, number );
	return 1;
    } else if( interrupt_count != 1 ) {
	fprintf( stderr, "%s: Test %d failed: Expected exception delivered %d times!\n",
		 testname, number, interrupt_count );
	return 1;
    } else if( interrupt_pc != expectedpc ) {
	fprintf( stderr, "%s: Test %d failed: Expected exception at PC %08X, but was %08X\n",
		 testname, number, expectedpc, interrupt_pc );
	return 1;
    } else {
	return 0;
    }
}

int main()
{
    fprintf( stdout, "Instruction tests...\n" );
    install_interrupt_handler();
    test_add();
    test_addc();
    test_addv();
    test_and();
    test_andi();
    test_bf(); 
    test_bt();
    test_bsr();
    test_cmp();
    test_cmpstr();
    test_div0();
    test_div1();
    test_float();
    test_fmov();
    test_ftrc();
    test_mac();
    test_rot();
    test_shl();
    test_shld();
    test_sub();
    test_subc();
    test_tas();
    test_xtrct();
    fprintf( stdout, "--> %d/%d instruction tests passed (%d%%)\n\n",
	     total_tests-total_fails, total_tests, 
	     ((total_tests-total_fails)*100)/total_tests );

    fprintf( stdout, "Exception tests...\n" );
    test_slot_illegal();
    test_undefined();
    remove_interrupt_handler();

    fprintf( stdout, "Total: %d/%d tests passed (%d%%)\n", total_tests-total_fails,
	     total_tests, ((total_tests-total_fails)*100)/total_tests );
    return total_fails;
}
