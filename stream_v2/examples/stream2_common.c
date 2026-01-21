/*
 * stream2_common.c - Platform compatibility implementation
 */
#include "stream2_common.h"

/* Global stop flag definition */
volatile sig_atomic_t g_stop = 0;
volatile sig_atomic_t g_out_of_space = 0;
