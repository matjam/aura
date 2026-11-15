/* API tests of src/css that do not fit the case files. */
#include "../../src/css/css_int.h"

#include <stdio.h>

int main(void)
{
    au_css_init();
    printf("ok\n");
    return 0;
}
