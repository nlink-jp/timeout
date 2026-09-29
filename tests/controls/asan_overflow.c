/* Positive control for layer 4 (ASan): must abort at run time. */
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
	(void)argc;
	char buf[4] = {0};
	memcpy(buf, argv[0], strlen(argv[0]));
	printf("%c\n", buf[0]);
	return 0;
}
