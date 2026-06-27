

#include <stdio.h>
#include <math.h>

int main(void)
{
    double s = 0.0;
    double sign = 1.0;
    int i;
    for (i = 0; i < 10000; i++) {
        s += sign / (2*i + 1);
        sign = -sign;
    }
    printf("pi ~ %.6f\n", 4.0 * s);
    return 0;
}
