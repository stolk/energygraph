#pragma once

#include "zones.h"

int  rapl_locate(_Atomic uint64_t* samples, char names[][ZONE_NAME_LEN], int* parents, int offset);
void rapl_stop(void);

