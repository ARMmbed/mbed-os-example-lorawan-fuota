/*
 * PackageLicenseDeclared: Apache-2.0
 * Copyright (c) 2018 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _DEV_EUI_HELPER_H
#define _DEV_EUI_HELPER_H

#include "mbed.h"

/**
 * This file reads DevEUI from ROM/RAM/Flash if the target has one.
 *
 * @param buffer    8 byte buffer to store the DevEUI in
 *                  if the target does not have a built-in DevEUI this buffer is not touched
 * @param size      Size of the buffer
 * @returns         0 if successful, -1 if no DevEUI could be read, -2 if the buffer size was not correct
 */

#if defined(TARGET_FF1705_L151CC) || defined(TARGET_XDOT_L151CC)
#include "xdot_eeprom.h"

int8_t get_built_in_dev_eui(uint8_t *buffer, size_t size) {
    if (size != 8) return -2;

    int v = xdot_eeprom_read_buf(0x401, buffer, size);
    if (v != 0) {
        return -1;
    }

    return 0;
}

#else
int8_t get_built_in_dev_eui(uint8_t *, size_t) {
    return -1;
}
#endif

#endif // _DEV_EUI_HELPER_H
