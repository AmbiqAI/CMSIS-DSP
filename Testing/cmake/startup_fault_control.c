/* Faults in a static constructor, before main() and before testmain_hook() starts the console. The run must
 * still print "E: HardFault" and end, instead of waiting for the timeout with nothing on the console. */

#include <stdio.h>

void testmain_hook(void);

__attribute__((constructor)) static void fault_before_main(void)
{
    __builtin_trap();
}

int main(void)
{
    testmain_hook();
    printf("STARTUP-FAULT-CONTROL REACHED MAIN\n");
    return 0;
}
