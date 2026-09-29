/* Positive control for layer 3 (-fbounds-safety): must trap at run time. */
#include <ptrcheck.h>
#include <stddef.h>
#include <stdio.h>
static int at(const int *__counted_by(n) p, size_t n, size_t i) { return p[i]; }
int main(int argc, char *__null_terminated *__counted_by(argc) argv)
{
	(void)argv;
	int a[3] = {1, 2, 3};
	printf("%d\n", at(a, 3, (size_t)argc + 2));
	return 0;
}
