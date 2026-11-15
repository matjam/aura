/* Flexbox (temporary: laid out as block flow until the flex algorithm lands). */
#include "layout_int.h"

float l_flex_layout(AuLayout *L, LBlock *b, const LConstraints *c)
{
    return l_flow_layout(L, b, c);
}

void l_flex_intrinsic(AuLayout *L, LBlock *b, float *min_c, float *max_c)
{
    l_flow_intrinsic(L, b, min_c, max_c);
}
