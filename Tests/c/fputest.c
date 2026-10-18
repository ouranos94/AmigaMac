/* FPUTest: floating point, compiled for the 68881 (tools/vcc -fpu=68881 ... -lm881)
 * and for the Mac; both outputs must match. */
#include <stdio.h>
#include <math.h>

static volatile double vx = 2.0, vy = 3.0;

int main(void)
{
    double a = vx, b = vy, s = 0;
    float f = (float)(a / b);
    printf("add %.12g sub %.12g mul %.12g div %.12g\n", a + b, a - b, a * b, a / b);
    printf("float %.6g\n", (double)f);
    printf("sqrt %.12g sin %.12g cos %.12g tan %.12g\n", sqrt(b), sin(a), cos(a), tan(a));
    printf("atan %.12g asin %.12g acos %.12g\n", atan(a), asin(0.5), acos(0.5));
    printf("exp %.12g log %.12g log10 %.12g pow %.12g\n", exp(a), log(b), log10(1000.0), pow(a, 10.0));
    printf("floor %.12g ceil %.12g fabs %.12g fmod %.12g\n", floor(-2.5), ceil(-2.5), fabs(-b), fmod(10.0, b));
    printf("sinh %.12g cosh %.12g tanh %.12g\n", sinh(1.0), cosh(1.0), tanh(0.5));
    for (int i = 1; i <= 1000; i++) s += 1.0 / ((double)i * i);
    printf("sum %.12g (pi^2/6 %.12g)\n", s, 3.14159265358979 * 3.14159265358979 / 6);
    long l = (long)(a * 1000.7), m = (long)(-b * 2.5);
    printf("int %ld %ld\n", l, m);
    printf("cmp %d %d %d %d\n", a < b, a > b, a == 2.0, b != 3.0);
    double big = 1e300 * 1e10, nan = big - big;
    printf("inf %d nan %d\n", isinf(big) != 0, isnan(nan) != 0);
    int k = 0;
    for (double x = 0; x < 1.0; x += 0.1) k++;
    printf("loop %d\n", k);
    return 0;
}
