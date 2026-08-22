#pragma once

#include "zones.h"

int  hwmo_locate(_Atomic uint64_t* samples, char names[][ZONE_NAME_LEN], int* parents, int offset);
void hwmo_stop(void);

