#include "../pad_battery.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    int m = 0;
    assert(!pad_battery_notice(-1, 0, &m) && !pad_battery_notice(50, 0, &m) && !pad_battery_notice(21, 0, &m));
    assert(pad_battery_notice(20, 0, &m) == 20 && m == 20);
    assert(!pad_battery_notice(19, 0, &m) && !pad_battery_notice(11, 0, &m));   // once per threshold
    assert(pad_battery_notice(10, 0, &m) == 10 && !pad_battery_notice(3, 0, &m));
    assert(!pad_battery_notice(3, 1, &m) && m == 0);   // charging arms them again
    assert(pad_battery_notice(15, 0, &m) == 20);
    assert(!pad_battery_notice(60, 0, &m) && m == 0);   // back above 20 % arms them again
    m = 0; assert(pad_battery_notice(8, 0, &m) == 10 && !pad_battery_notice(15, 0, &m));   // first read already very low: only the 10 % notice
    puts("t_pad_battery: passed");
}
