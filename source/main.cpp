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

#include "mbed.h"
#include "mbed_trace.h"
#include "mbed_mem_trace.h"
#include "LoRaWANInterface.h"
#include "lora_radio_helper.h"
#include "dev_eui_helper.h"
#include "storage_helper.h"
#include "UpdateCerts.h"
#include "LoRaWANUpdateClient.h"
#include "FragAssembler.h"
#include "MulticastControlPackage.h"
#include "ClockSyncControlPackage.h"
#include "FragmentationControlPackage.h"

#define TRACE_GROUP "MAIN"

EventQueue evqueue;

// Note: if the device has built-in dev eui (see dev_eui_helper.h), the dev eui will be overwritten in main()
static uint8_t DEV_EUI[] = { 0x00, 0x80, 0x00, 0x00, 0x04, 0x00, 0x04, 0xC9 };
static uint8_t APP_EUI[] = { 0x70, 0xB3, 0xD5, 0x7E, 0xD0, 0x00, 0xC1, 0x84 };
static uint8_t APP_KEY[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };

static void lora_event_handler(lorawan_event_t event);
static void lora_uc_send(LoRaWANUpdateClientSendParams_t &params);
static void queue_next_send_message();
static void send_message();

static LoRaWANInterface lorawan(radio);
static lorawan_app_callbacks_t callbacks;

static LoRaWANUpdateClient uc(&bd, APP_KEY, lora_uc_send);
static bool in_class_c_mode = false;
static LoRaWANUpdateClientSendParams_t queued_message;
static bool queued_message_waiting = false;

// @todo: actually should be able to handle multiple frag_index'es but not now


#if MBED_CONF_LORAWAN_UPDATE_CLIENT_INTEROP_TESTING
uint32_t interop_crc32 = 0x0;
#endif


static DigitalOut led1(ACTIVITY_LED);

static void turn_led_on() {
    if (ACTIVITY_LED == NC) return;
    led1 = 1;
}
static void turn_led_off() {
    if (ACTIVITY_LED == NC) return;
    led1 = 0;
}

// This is already debounced to the eventqueue, so safe to run printf here
static void switch_to_class_a() {
    printf("Switch to Class A\n");
    turn_led_off();
    uc.printHeapStats("CLASSA ");

    in_class_c_mode = false;

    // put back the class A session
    lorawan.enable_adaptive_datarate();
    lorawan.restore_rx2_frequency_and_dr();
    lorawan.set_device_class(CLASS_A);

    // wait for a few seconds to send the message
    evqueue.call_in(5000, &send_message);
}

static void switch_to_class_c(uint32_t life_time, uint8_t dr, uint32_t dl_freq) {
    printf("Switch to Class C\n");
    turn_led_on();

    // cancel all pending class A messages
    lorawan.cancel_sending();
    lorawan.disable_adaptive_datarate();

    if (queued_message_waiting) {
        queued_message_waiting = false;
        free(queued_message.data);
    }

    if (lorawan.set_rx2_frequency_and_dr(dl_freq, dr) != LORAWAN_STATUS_OK) {
        printf("Failed to set up frequency %lu and dr %u for Class C session\n", dl_freq, dr);
    } else {
        printf("Class C session frequency: %lu, dr: %u\n", dl_freq, dr);
    }

    lorawan.set_device_class(CLASS_C);

    in_class_c_mode = true;

    // switch back to class A after the timeout
    evqueue.call_in(life_time * 1000, switch_to_class_a);
}

static void switch_class(uint8_t device_class,
                         uint32_t time_to_switch,
                         uint32_t life_time,
                         uint8_t dr,
                         uint32_t dl_freq)
{
    if (device_class == CLASS_A) {
        printf("Switching to class A in %lu seconds\n", time_to_switch);
        evqueue.call_in(time_to_switch * 1000, switch_to_class_a);
    }
    else if (device_class == CLASS_C) {
        printf("Switching to class C in %lu seconds\n", time_to_switch);
        evqueue.call_in(time_to_switch * 1000, switch_to_class_c, life_time, dr, dl_freq);
    }
    else {
        printf("Switching to unknown class %u?!\n", device_class);
    }
}

LW_UC_STATUS verifyAuthenticityAndWriteBootloader(uint32_t addr, UpdateSignature_t *header, size_t flashOffset, size_t flashLength);
LW_UC_STATUS writeBootloaderHeader(uint32_t addr, uint32_t version, size_t fwSize, unsigned char sha_hash[32]);
LW_UC_STATUS applySlot0Slot2DeltaUpdate(size_t sizeOfFwInSlot0, size_t sizeOfFwInSlot2, uint32_t *sizeOfFwInSlot1);

/**
 * Verify the authenticity (SHA hash and ECDSA hash) of a firmware package,
 * and after passing verification write the bootloader header
 *
 * @param addr Address of firmware slot (MBED_CONF_LORAWAN_UPDATE_CLIENT_SLOT0_HEADER_ADDRESS or MBED_CONF_LORAWAN_UPDATE_CLIENT_SLOT1_HEADER_ADDRESS)
 * @param header Firmware manifest
 * @param flashOffset Offset in flash of the firmware
 * @param flashLength Length in flash of the firmware
 */
LW_UC_STATUS verifyAuthenticityAndWriteBootloader(uint32_t addr, UpdateSignature_t *header, size_t flashOffset, size_t flashLength) {

    if (!uc.compare_buffers(header->manufacturer_uuid, UPDATE_CERT_MANUFACTURER_UUID, 16)) {
        return LW_UC_SIGNATURE_MANUFACTURER_UUID_MISMATCH;
    }

    if (!uc.compare_buffers(header->device_class_uuid, UPDATE_CERT_DEVICE_CLASS_UUID, 16)) {
        return LW_UC_SIGNATURE_DEVICECLASS_UUID_MISMATCH;
    }

    printf("Verification starting...\n");

    // if (callbacks.verificationStarting) {
    //     callbacks.verificationStarting();
    // }

    // Calculate the SHA256 hash of the file, and then verify whether the signature was signed with a trusted private key
    unsigned char sha_out_buffer[32];
    // Internal buffer for reading from BD
    uint8_t sha_buffer[LW_UC_SHA256_BUFFER_SIZE];

    // SHA256 requires a large buffer, alloc on heap instead of stack
    FragmentationSha256* sha256 = new FragmentationSha256(&bdWrapper, sha_buffer, sizeof(sha_buffer));

    sha256->calculate(flashOffset, flashLength, sha_out_buffer);

    delete sha256;

    tr_debug("New firmware SHA256 hash is: ");
    for (size_t ix = 0; ix < 32; ix++) {
        printf("%02x", sha_out_buffer[ix]);
    }
    printf("\n");

    // now check that the signature is correct...
    {
        tr_debug("ECDSA signature is: ");
        for (size_t ix = 0; ix < header->signature_length; ix++) {
            printf("%02x", header->signature[ix]);
        }
        printf("\n");
        tr_debug("Verifying signature...");

        // ECDSA requires a large buffer, alloc on heap instead of stack
        FragmentationEcdsaVerify* ecdsa = new FragmentationEcdsaVerify(UPDATE_CERT_PUBKEY, UPDATE_CERT_LENGTH);
        bool valid = ecdsa->verify(sha_out_buffer, header->signature, header->signature_length);

        delete ecdsa;

        // if (callbacks.verificationFinished) {
        //     callbacks.verificationFinished();
        // }
        printf("Verification finished...\n");

        if (!valid) {
            tr_warn("New firmware signature verification failed");
            return LW_UC_SIGNATURE_ECDSA_FAILED;
        }
        else {
            tr_debug("New firmware signature verification passed");
        }
    }

    return writeBootloaderHeader(addr, header->version, flashLength, sha_out_buffer);
}

/**
 * Write the bootloader header so the firmware can be flashed
 *
 * @param addr Beginning of the firmware slot (e.g. MBED_CONF_LORAWAN_UPDATE_CLIENT_SLOT0_HEADER_ADDRESS)
 * @param version Build timestamp of the firmware
 * @param fwSize Size of the firmware in bytes
 * @param sha_hash SHA256 hash of the firmware
 *
 * @returns LW_UC_OK if all went well, or non-0 status when something went wrong
 */
LW_UC_STATUS writeBootloaderHeader(uint32_t addr, uint32_t version, size_t fwSize, unsigned char sha_hash[32]) {
    if (addr != MBED_CONF_LORAWAN_UPDATE_CLIENT_SLOT0_HEADER_ADDRESS && addr != MBED_CONF_LORAWAN_UPDATE_CLIENT_SLOT1_HEADER_ADDRESS) {
        return LW_UC_INVALID_SLOT;
    }

    arm_uc_firmware_details_t details;

    // this is useful for tests, when the firmware is always older
#if MBED_CONF_LORAWAN_UPDATE_CLIENT_OVERWRITE_VERSION == 1
    // read internal flash page to see what version we're at
    uint64_t currVersion;
    LW_UC_STATUS status = getCurrentVersion(&currVersion);
    if (status != LW_UC_OK) {
        // fallback
        currVersion = (uint64_t)MBED_BUILD_TIMESTAMP;
    }
    details.version = currVersion + 1;
#else
    details.version = static_cast<uint64_t>(version);
#endif

    details.size = fwSize;
    memcpy(details.hash, sha_hash, 32); // SHA256 hash of the firmware
    memset(details.campaign, 0, ARM_UC_GUID_SIZE); // todo, add campaign info
    details.signatureSize = 0; // not sure what this is used for

    tr_debug("writeBootloaderHeader:\n\taddr: %lu\n\tversion: %llu\n\tsize: %llu", addr, details.version, details.size);

    uint8_t *fw_header_buff = (uint8_t*)malloc(ARM_UC_EXTERNAL_HEADER_SIZE_V2);
    if (!fw_header_buff) {
        tr_error("Could not allocate %d bytes for header", ARM_UC_EXTERNAL_HEADER_SIZE_V2);
        return LW_UC_OUT_OF_MEMORY;
    }

    arm_uc_buffer_t buff = { ARM_UC_EXTERNAL_HEADER_SIZE_V2, ARM_UC_EXTERNAL_HEADER_SIZE_V2, fw_header_buff };

    arm_uc_error_t err = arm_uc_create_external_header_v2(&details, &buff);

    if (err.error != ERR_NONE) {
        tr_error("Failed to create external header (%d)", err.error);
        free(fw_header_buff);
        return LW_UC_CREATE_BOOTLOADER_HEADER_FAILED;
    }

    int r = bdWrapper.program(buff.ptr, addr, buff.size);
    if (r != BD_ERROR_OK) {
        tr_error("Failed to program firmware header: %lu bytes at address 0x%lx", buff.size, addr);
        free(fw_header_buff);
        return LW_UC_BD_WRITE_ERROR;
    }

    tr_debug("Stored the update parameters in flash on 0x%lx. Reset the board to apply update.", addr);

    free(fw_header_buff);

    return LW_UC_OK;
}

#if MBED_CONF_LORAWAN_UPDATE_CLIENT_OVERWRITE_VERSION == 1
/**
 * Get the current version number of the application from internal flash
 */
LW_UC_STATUS getCurrentVersion(uint64_t* version) {
#if DEVICE_FLASH
    int r;
    if ((r = _internalFlash.init()) != 0) {
        tr_warn("Could not initialize internal flash (%d)", r);
        return LW_UC_INTERNALFLASH_INIT_ERROR;
    }

    uint32_t sectorSize = _internalFlash.get_sector_size(MBED_CONF_LORAWAN_UPDATE_CLIENT_INTERNAL_FLASH_HEADER);
    tr_debug("Internal flash sectorSize is %lu", sectorSize);

    if (sectorSize < ARM_UC_INTERNAL_HEADER_SIZE_V2) {
        tr_warn("SectorSize is smaller than ARM_UC_INTERNAL_HEADER_SIZE_V2 (%lu), cannot handle this", sectorSize);
        return LW_UC_INTERNALFLASH_SECTOR_SIZE_SMALLER;
    }

    uint8_t *buffer = (uint8_t*)malloc(sectorSize);
    if (!buffer) {
        tr_warn("getCurrentVersion() - Could not allocate %lu bytes", sectorSize);
        return LW_UC_OUT_OF_MEMORY;
    }

    if ((r = _internalFlash.read(buffer,  MBED_CONF_LORAWAN_UPDATE_CLIENT_INTERNAL_FLASH_HEADER, sectorSize)) != 0) {
        tr_warn("Read on internal flash failed (%d)", r);
        free(buffer);
        return LW_UC_INTERNALFLASH_READ_ERROR;
    }

    if ((r = _internalFlash.deinit()) != 0) {
        tr_warn("Could not de-initialize internal flash (%d)", r);
        free(buffer);
        return LW_UC_INTERNALFLASH_DEINIT_ERROR;
    }

    arm_uc_firmware_details_t details;

    arm_uc_error_t err = arm_uc_parse_internal_header_v2(const_cast<uint8_t*>(buffer), &details);
    if (err.error != ERR_NONE) {
        tr_warn("Internal header parsing failed (%d)", err.error);
        free(buffer);
        return LW_UC_INTERNALFLASH_HEADER_PARSE_FAILED;
    }

    *version = details.version;
    tr_debug("Version (from internal flash) is %llu", details.version);
    free(buffer);
    return LW_UC_OK;
#else
    *version = (uint64_t)MBED_BUILD_TIMESTAMP;
    return LW_UC_OK;
#endif
}
#endif

/**
 * Apply a delta update between slot 2 (source file) and slot 0 (diff file) and place in slot 1
 *
 * @param sizeOfFwInSlot0 Size of the diff image that we just received
 * @param sizeOfFwInSlot2 Expected size of firmware in slot 2 (will do sanity check)
 * @param sizeOfFwInSlot1 Out parameter which will be set to the size of the new firmware in slot 1
 */
LW_UC_STATUS applySlot0Slot2DeltaUpdate(size_t sizeOfFwInSlot0, size_t sizeOfFwInSlot2, uint32_t *sizeOfFwInSlot1) {
    // read details about the current firmware, it's in the slot2 header
    arm_uc_firmware_details_t curr_details;
    int bd_status = bdWrapper.read(&curr_details, MBED_CONF_LORAWAN_UPDATE_CLIENT_SLOT2_HEADER_ADDRESS, sizeof(arm_uc_firmware_details_t));
    if (bd_status != BD_ERROR_OK) {
        return LW_UC_BD_READ_ERROR;
    }

    // so... sanity check, do we have the same size in both places
    if (sizeOfFwInSlot2 != curr_details.size) {
        tr_warn("Diff size mismatch, expecting %u (manifest) but got %llu (slot 2 content)", sizeOfFwInSlot2, curr_details.size);
        return LW_UC_DIFF_SIZE_MISMATCH;
    }

    // calculate sha256 hash for current fw & diff file (for debug purposes)
    {
        unsigned char sha_out_buffer[32];
        uint8_t sha_buffer[LW_UC_SHA256_BUFFER_SIZE];
        FragmentationSha256* sha256 = new FragmentationSha256(&bdWrapper, sha_buffer, sizeof(sha_buffer));

        tr_debug("Firmware hash in slot 2 (current firmware): ");
        sha256->calculate(MBED_CONF_LORAWAN_UPDATE_CLIENT_SLOT2_FW_ADDRESS, sizeOfFwInSlot2, sha_out_buffer);
        uc.print_buffer(sha_out_buffer, 32, false);
        printf("\n");

        tr_debug("Firmware hash in slot 2 (expected): ");
        uc.print_buffer(curr_details.hash, 32, false);
        printf("\n");

        if (!uc.compare_buffers(curr_details.hash, sha_out_buffer, 32)) {
            tr_info("Firmware in slot 2 hash incorrect hash");
            delete sha256;
            return LW_UC_DIFF_INCORRECT_SLOT2_HASH;
        }

        tr_debug("Firmware hash in slot 0 (diff file): ");
        sha256->calculate(MBED_CONF_LORAWAN_UPDATE_CLIENT_SLOT0_FW_ADDRESS, sizeOfFwInSlot0, sha_out_buffer);
        uc.print_buffer(sha_out_buffer, 32, false);
        printf("\n");

        delete sha256;
    }

    // now run the diff...
    BDFILE source(&bdWrapper, MBED_CONF_LORAWAN_UPDATE_CLIENT_SLOT2_FW_ADDRESS, sizeOfFwInSlot2);
    BDFILE diff(&bdWrapper, MBED_CONF_LORAWAN_UPDATE_CLIENT_SLOT0_FW_ADDRESS, sizeOfFwInSlot0);
    BDFILE target(&bdWrapper, MBED_CONF_LORAWAN_UPDATE_CLIENT_SLOT1_FW_ADDRESS, 0);

    int v = apply_delta_update(&bdWrapper, LW_UC_JANPATCH_BUFFER_SIZE, &source, &diff, &target);

    if (v != MBED_DELTA_UPDATE_OK) {
        tr_warn("apply_delta_update failed %d", v);
        return LW_UC_DIFF_DELTA_UPDATE_FAILED;
    }

    tr_debug("Patched firmware length is %ld", target.ftell());

    *sizeOfFwInSlot1 = target.ftell();

    return LW_UC_OK;
}

static void lorawan_uc_fragsession_complete(uint8_t frag_index) {
    printf("Frag session is complete\n");

    switch_to_class_a();
    // should cancel the evqueue.call_in here too I guess?

    lorawan_frag_session_t *session = frag_plugin.get_frag_session(frag_index);
    if (!session) {
        printf("Could not fetch session?! %u\n", frag_index);
        return;
    }

    size_t totalSize = (static_cast<size_t>(session->nb_frag * session->frag_size) - session->padding);

#if MBED_CONF_LORAWAN_UPDATE_CLIENT_INTEROP_TESTING
    // Internal buffer for reading from BD
    uint8_t crc_buffer[LW_UC_SHA256_BUFFER_SIZE];

    printf("addr: %x, size: %lu\n",
            MBED_CONF_LORAWAN_UPDATE_CLIENT_SLOT0_FW_ADDRESS,
            totalSize);

    uint8_t buff[995];
    bdWrapper.read(buff, MBED_CONF_LORAWAN_UPDATE_CLIENT_SLOT0_FW_ADDRESS, 995);

    for (size_t ix = 0; ix < sizeof(buff); ix++) {
        printf("%02x ", buff[ix]);
    }
    printf("\n");

    FragmentationCrc32 crc32(&bdWrapper, crc_buffer, LW_UC_SHA256_BUFFER_SIZE);
    uint32_t crc = crc32.calculate(MBED_CONF_LORAWAN_UPDATE_CLIENT_SLOT0_FW_ADDRESS, totalSize);
    printf("Firmware is ready, CRC32 hash is %08lx\n", crc);
    interop_crc32 = crc;
#else

    // the signature is the last FOTA_SIGNATURE_LENGTH bytes of the package
    size_t signatureOffset = MBED_CONF_LORAWAN_UPDATE_CLIENT_SLOT0_FW_ADDRESS + (static_cast<size_t>(session->nb_frag * session->frag_size) - session->padding) - FOTA_SIGNATURE_LENGTH;

    // Manifest to read in
    UpdateSignature_t header;
    if (bdWrapper.read(&header, signatureOffset, FOTA_SIGNATURE_LENGTH) != BD_ERROR_OK) {
        printf("Reading sig failed...\n");
        return;
        // return LW_UC_BD_READ_ERROR;
    }

    // So... now it depends on whether this is a delta update or not...
    uint8_t* diff_info = (uint8_t*)&(header.diff_info);

    printf("Diff info: is_diff=%u, size_of_old_fw=%u\b", diff_info[0], (diff_info[1] << 16) + (diff_info[2] << 8) + diff_info[3]);

    if (diff_info[0] == 0) { // Not a diff...
        // last FOTA_SIGNATURE_LENGTH bytes should be ignored because the signature is not part of the firmware
        size_t fwSize = (static_cast<size_t>(session->nb_frag * session->frag_size) - session->padding) - FOTA_SIGNATURE_LENGTH;
        LW_UC_STATUS authStatus = verifyAuthenticityAndWriteBootloader(
            MBED_CONF_LORAWAN_UPDATE_CLIENT_SLOT0_HEADER_ADDRESS,
            &header,
            MBED_CONF_LORAWAN_UPDATE_CLIENT_SLOT0_FW_ADDRESS,
            fwSize);

        if (authStatus != LW_UC_OK) {
            printf("Firmware verification failed (%d)\n", authStatus);
            //return authStatus;
            return;
        }

        // if (callbacks.firmwareReady) {
        //     callbacks.firmwareReady();
        // }
        printf("FIRMWARE IS READY!!\n");

        NVIC_SystemReset();

        return /*LW_UC_OK*/;
    }
    else {
        uint32_t slot1Size;
        LW_UC_STATUS deltaStatus = applySlot0Slot2DeltaUpdate(
            (static_cast<size_t>(session->nb_frag * session->frag_size) - session->padding) - FOTA_SIGNATURE_LENGTH,
            (diff_info[1] << 16) + (diff_info[2] << 8) + diff_info[3],
            &slot1Size
        );

        if (deltaStatus != LW_UC_OK) {
            printf("Delta update failed (%d)\n", deltaStatus);
            // return deltaStatus;
            return;
        }

        LW_UC_STATUS authStatus = verifyAuthenticityAndWriteBootloader(
            MBED_CONF_LORAWAN_UPDATE_CLIENT_SLOT1_HEADER_ADDRESS,
            &header,
            MBED_CONF_LORAWAN_UPDATE_CLIENT_SLOT1_FW_ADDRESS,
            slot1Size);

        if (authStatus != LW_UC_OK) {
            printf("Firmware verification failed (%d)\n", authStatus);
            //return authStatus;
            return;
        }

        // if (callbacks.firmwareReady) {
        //     callbacks.firmwareReady();
        // }
        printf("FIRMWARE IS READY\n");

        NVIC_SystemReset();

        return /*LW_UC_OK*/;
    }

#endif
}

LW_UC_STATUS updateClassCSessionAns(LoRaWANUpdateClientSendParams_t *queued_message) {
    if (queued_message->port != MCCONTROL_PORT || queued_message->length != MC_CLASSC_SESSION_ANS_LENGTH
            || queued_message->data[0] != MC_CLASSC_SESSION_ANS) {
        return LW_UC_NOT_CLASS_C_SESSION_ANS;
    }

    uint32_t originalTimeToStart = queued_message->data[2] + (queued_message->data[3] << 8) + (queued_message->data[4] << 16);

    // calculate delta between original send time and now
    uint32_t timeDelta = time(NULL) - queued_message->createdTimestamp;

    uint32_t timeToStart;
    if (timeDelta > originalTimeToStart) { // should already have started, send 0 back
        timeToStart = 0;
    }
    else {
        timeToStart = originalTimeToStart - timeDelta;
    }

    tr_debug("updateClassCSessionAns, originalTimeToStart=%lu, delta=%lu, newTimeToStart=%lu",
        originalTimeToStart, timeDelta, timeToStart);

    // update buffer
    queued_message->data[2] = timeToStart & 0xff;
    queued_message->data[3] = timeToStart >> 8 & 0xff;
    queued_message->data[4] = timeToStart >> 16 & 0xff;

    return LW_UC_OK;
}

static void lora_uc_send(clk_sync_response_t *resp) {
    queued_message.port = CLOCK_SYNC_PORT;
    queued_message.length = resp->size;
    queued_message.confirmed = false; // @todo
    queued_message.retriesAllowed = resp->forced_resync_required;
    queued_message.createdTimestamp = time(NULL);

    queued_message.data = (uint8_t*)malloc(resp->size);
    if (!queued_message.data) {
        printf("ERR! Failed to allocate %u bytes for queued_message!\n", resp->size);
        return;
    }
    memcpy(queued_message.data, resp->data, resp->size);
    queued_message_waiting = true;

    // will be sent in the next iteration
}

static void lora_uc_send(mcast_ctrl_response_t *resp) {
    queued_message.port = MULTICAST_CONTROL_PORT;
    queued_message.length = resp->size;
    queued_message.confirmed = false; // @todo
    queued_message.retriesAllowed = true;
    queued_message.createdTimestamp = time(NULL);

    queued_message.data = (uint8_t*)malloc(resp->size);
    if (!queued_message.data) {
        printf("ERR! Failed to allocate %u bytes for queued_message!\n", resp->size);
        return;
    }
    memcpy(queued_message.data, resp->data, resp->size);
    queued_message_waiting = true;

    // will be sent in the next iteration
}

static void lora_uc_send(frag_cmd_answer_t *resp) {
    queued_message.port = FRAGMENTATION_CONTROL_PORT;
    queued_message.length = resp->size;
    queued_message.confirmed = false; // @todo
    queued_message.retriesAllowed = true;
    queued_message.createdTimestamp = time(NULL);

    queued_message.data = (uint8_t*)malloc(resp->size);
    if (!queued_message.data) {
        printf("ERR! Failed to allocate %u bytes for queued_message!\n", resp->size);
        return;
    }
    memcpy(queued_message.data, resp->data, resp->size);
    queued_message_waiting = true;

    // will be sent in the next iteration
}

static void lora_uc_send(LoRaWANUpdateClientSendParams_t &params) {
    queued_message = params;
    // copy the buffer
    queued_message.data = (uint8_t*)malloc(params.length);
    if (!queued_message.data) {
        printf("ERR! Failed to allocate %u bytes for queued_message!\n", params.length);
        return;
    }
    memcpy(queued_message.data, params.data, params.length);
    queued_message_waiting = true;

    // will be sent in the next iteration
}

// Send a message over LoRaWAN - todo, check for duty cycle
static void send_message() {
    if (in_class_c_mode) return;

#if MBED_CONF_LORAWAN_UPDATE_CLIENT_INTEROP_TESTING
    // after calculating the crc32, that's the only thing we'll send
    if (interop_crc32 != 0x0) {
        uint8_t buffer[6] = {
            DATA_BLOCK_AUTH_REQ, 0 /* fragIndex, always 0 */,
            interop_crc32 & 0xff, interop_crc32 >> 8 & 0xff, interop_crc32 >> 16 & 0xff, interop_crc32 >> 24 & 0xff
        };
        int16_t retcode = lorawan.send(201, buffer, sizeof(buffer), MSG_UNCONFIRMED_FLAG);
        if (retcode < 0) {
            printf("send_message for DATA_BLOCK_AUTH_REQ on port %d failed (%d)\n", 201, retcode);
            queue_next_send_message();
        }
        else {
            printf("%d bytes scheduled for transmission on port %d\n", sizeof(buffer), 201);
        }
        return;
    }
#endif

    // @todo: implement retries allowed
    if (queued_message_waiting) {
        // detect if this is class c session start message
        // because if so, we should change the timeToStart to the current moment as we don't send immediately
        if (queued_message.port == MCCONTROL_PORT && queued_message.length == MC_CLASSC_SESSION_ANS_LENGTH
                && queued_message.data[0] == MC_CLASSC_SESSION_ANS) {
            updateClassCSessionAns(&queued_message);
        }

        int16_t retcode = lorawan.send(
            queued_message.port,
            queued_message.data,
            queued_message.length,
            queued_message.confirmed ? MSG_CONFIRMED_FLAG : MSG_UNCONFIRMED_FLAG);

        if (retcode < 0) {
            printf("send_message for queued_message on port %d failed (%d)\n", queued_message.port, retcode);
            queue_next_send_message();
        }
        else {
            free(queued_message.data);
            queued_message_waiting = false;
            printf("%d bytes scheduled for transmission on port %d\n", queued_message.length, queued_message.port);
        }

        return;
    }

    if (lorawan.get_current_gps_time() < 1000000000) {
        clk_sync_response_t *sync_resp = clk_sync_plugin.request_clock_sync(true);

        if (sync_resp) {
            printf("Sending clock sync now\n");
            lora_uc_send(sync_resp);
            send_message();
            return;
        }
        else {
            printf("Failed to request clock sync from clk_sync_plugin...\n");
        }
    }

    // otherwise just send a random message (this is where you'd put your sensor data)
    int r = rand();
    int16_t retcode = lorawan.send(15, (uint8_t*)(&r), sizeof(r), MSG_UNCONFIRMED_FLAG);

    if (retcode < 0) {
        printf("send_message for normal message on port %d failed (%d)\n", 15, retcode);
        queue_next_send_message();
    }
    else {
        printf("%d bytes scheduled for transmission on port %d\n", sizeof(r), 15);
    }
}

static void queue_next_send_message() {
    if (in_class_c_mode) return;

    int backoff;
    lorawan.get_backoff_metadata(backoff);

    if (backoff < 0) {
        backoff = 5000;
    }

    evqueue.call_in(backoff, &send_message);
}

int main() {
    printf("\nMbed OS 5 Firmware Update over LoRaWAN\n");

    // mbed_mem_trace_set_callback(mbed_mem_trace_default_callback);

    // Enable trace output for this demo, so we can see what the LoRaWAN stack does
    mbed_trace_init();
    mbed_trace_exclude_filters_set("QSPIF");



    if (lorawan.initialize(&evqueue) != LORAWAN_STATUS_OK) {
        printf("LoRa initialization failed!\n");
        return -1;
    }

    // prepare application callbacks
    callbacks.events = callback(lora_event_handler);
    lorawan.add_app_callbacks(&callbacks);

    // Enable adaptive data rating
    if (lorawan.enable_adaptive_datarate() != LORAWAN_STATUS_OK) {
        printf("enable_adaptive_datarate failed!\n");
        return -1;
    }

    lorawan.set_device_class(CLASS_A);

    if (get_built_in_dev_eui(DEV_EUI, sizeof(DEV_EUI)) == 0) {
        printf("read built-in dev eui: %02x %02x %02x %02x %02x %02x %02x %02x\n",
            DEV_EUI[0], DEV_EUI[1], DEV_EUI[2], DEV_EUI[3], DEV_EUI[4], DEV_EUI[5], DEV_EUI[6], DEV_EUI[7]);
    }

    lorawan_connect_t connect_params;
    connect_params.connect_type = LORAWAN_CONNECTION_OTAA;
    connect_params.connection_u.otaa.dev_eui = DEV_EUI;
    connect_params.connection_u.otaa.app_eui = APP_EUI;
    connect_params.connection_u.otaa.app_key = APP_KEY;
    connect_params.connection_u.otaa.nb_trials = 3;

    lorawan_status_t retcode = lorawan.connect(connect_params);

    if (retcode == LORAWAN_STATUS_OK ||
        retcode == LORAWAN_STATUS_CONNECT_IN_PROGRESS) {
    } else {
        printf("Connection error, code = %d\n", retcode);
        return -1;
    }

    printf("Connection - In Progress ...\r\n");

    // make your event queue dispatching events forever
    evqueue.dispatch_forever();
}

// This is called from RX_DONE, so whenever a message came in
static void receive_message() {
    uint8_t rx_buffer[255] = { 0 };
    uint8_t port;
    int flags;
    int16_t retcode = lorawan.receive(rx_buffer, sizeof(rx_buffer), port, flags);

    if (retcode < 0) {
        printf("receive() - Error code %d\n", retcode);
        return;
    }

    printf("Received %d bytes on port %u\n", retcode, port);

    LW_UC_STATUS status = LW_UC_OK;

    if (port == 200) {
        uc.handleMulticastControlCommand(rx_buffer, retcode);
    }
    else if (port == 201) {
        uc.handleFragmentationCommand(rx_buffer, retcode);

        // blink LED when receiving a packet in Class C mode
        if (in_class_c_mode) {
            turn_led_on();
            evqueue.call_in(200, &turn_led_off);
        }
    }
    else if (port == 202) {
        uc.handleClockSyncCommand(rx_buffer, retcode);
    }
    else {
        printf("Data received on port %d (length %d): ", port, retcode);

        for (uint8_t i = 0; i < retcode; i++) {
            printf("%02x ", rx_buffer[i]);
        }
        printf("\n");
    }

    if (status != LW_UC_OK) {
        printf("Failed to handle UC command on port %d, status %d\n", port, status);
    }
}

// Event handler
static void lora_event_handler(lorawan_event_t event) {
    switch (event) {
        case CONNECTED:
            printf("Connection - Successful\n");

            uc.printHeapStats("CONNECTED ");

            queue_next_send_message();
            break;
        case DISCONNECTED:
            printf("Disconnected Successfully\n");
            break;
        case TX_DONE:
        {
            printf("Message Sent to Network Server\n");
            queue_next_send_message();
            break;
        }
        case TX_TIMEOUT:
        case TX_ERROR:
        case TX_CRYPTO_ERROR:
        case TX_SCHEDULING_ERROR:
            printf("Transmission Error - EventCode = %d\n", event);
            queue_next_send_message();
            break;
        case RX_DONE:
            receive_message();
            break;
        case RX_TIMEOUT:
        case RX_ERROR:
            printf("Error in reception - Code = %d\n", event);
            break;
        case JOIN_FAILURE:
            printf("OTAA Failed - Check Keys\n");
            break;
        default:
            MBED_ASSERT("Unknown Event");
            break;
    }
}
