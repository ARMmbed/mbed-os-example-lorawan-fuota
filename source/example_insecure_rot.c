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

#if !defined(ARM_UC_USE_SOTP) || ARM_UC_USE_SOTP == 0

#include <inttypes.h>
#include <stddef.h>

#define DEVICE_KEY_SIZE_IN_BYTES (128/8)

/**
 * @brief Function to get the device root of trust
 * @details The device root of trust should be a 128 bit value. It should never leave the device.
 *          It should be unique to the device. It should have enough entropy to avoid contentional
 *          entropy attacks. The porter should implement the following device signature to provide
 *          device root of trust on different platforms.
 *
 * @param key_buf buffer to be filled with the device root of trust.
 * @param length  length of the buffer provided to make sure no overflow occurs.
 *
 * @return 0 on success, non-zero on failure.
 */

// THIS CODE IS FOR TESTING PURPOSES ONLY. DO NOT USE IN PRODUCTION ENVIRONMENTS. REPLACE WITH A PROPER IMPLEMENTATION BEFORE USE
int8_t mbed_cloud_client_get_rot_128bit(uint8_t *key_buf, uint32_t length)
{
#warning "You are using insecure Root Of Trust implementation, DO NOT USE IN PRODUCTION ENVIRONMENTS. REPLACE WITH A PROPER IMPLEMENTATION BEFORE USE"

    if (length < DEVICE_KEY_SIZE_IN_BYTES || key_buf == NULL)
    {
        return -1;
    }

    for (uint8_t i = 0; i < DEVICE_KEY_SIZE_IN_BYTES; i++)
    {
        key_buf[i] = i;
    }

    return 0;
}

#endif // #if !defined(ARM_UC_USE_SOTP) || ARM_UC_USE_SOTP == 0
