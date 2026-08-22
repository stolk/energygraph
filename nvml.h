#pragma once

#include "zones.h"

int  nvml_locate(_Atomic uint64_t* samples, char names[][ZONE_NAME_LEN], int* parents, int offset);
void nvml_stop(void);

