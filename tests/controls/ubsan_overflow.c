/* Positive control for layer 4 (UBSan, no recovery): must fail at run time. */
#include <limits.h>
#include <stdio.h>
int main(int argc, char **argv)
{
	(void)argv;
	int x = INT_MAX;
	x += argc;
	printf("%d\n", x);
	return 0;
}
