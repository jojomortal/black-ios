#include <stdio.h>
#include "spike.h"
int main(void) { static char b[8192]; fputs(spike_run(b, sizeof b, 0), stdout); return 0; }
