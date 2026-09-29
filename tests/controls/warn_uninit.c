/* Positive control for layer 1 (warnings): must fail to compile. */
int control(int c);
int control(int c) { int v; if (c) v = 1; return v; }
