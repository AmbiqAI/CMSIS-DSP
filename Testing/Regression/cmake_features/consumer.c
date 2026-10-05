/* Copyright (c) 2026 Ambiq Micro, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include "features.h"
extern unsigned library_features(void);
int main(void)
{
    const unsigned library = library_features();
    const unsigned consumer = configured_features();
    if (library != EXPECTED_FEATURES || consumer != library)
    {
        printf("FAIL: library=%u consumer=%u expected=%u\n", library, consumer,
               (unsigned)EXPECTED_FEATURES);
        return 1;
    }
    return 0;
}
