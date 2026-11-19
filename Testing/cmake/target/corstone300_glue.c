/* Corstone-300 run-time support for the CMSIS-DSP test images. The console is
 * Testing/Regression/corstone300_console.c. */

void regression_console_init(void);
void testmain_hook(void);

/* testmain.cpp calls this weak hook before any output. */
void testmain_hook(void)
{
    regression_console_init();
}
