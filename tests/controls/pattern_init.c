/* Positive control for layer 4 (pattern auto-var init): the uninitialized
 * read must come back as the fill pattern, not as zero or stack garbage. */
#include <stdio.h>
static unsigned read_uninit(int c) { unsigned v; if (c > 100) v = 1; return v; }
int main(int argc, char **argv)
{
	(void)argv;
	printf("%08x\n", read_uninit(argc));
	return 0;
}
