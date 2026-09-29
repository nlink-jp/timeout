/* Positive control for layer 2 (static analysis): must be reported. */
char *control(void);
char *control(void) { char b[8]; b[0] = 0; char *p = b; return p; }
