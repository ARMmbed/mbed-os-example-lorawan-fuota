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

EventQueue evqueue;

// Note: if the device has built-in dev eui (see dev_eui_helper.h), the dev eui will be overwritten in main()
static uint8_t DEV_EUI[] = { 0x00, 0x80, 0x00, 0x00, 0x04, 0x00, 0x39, 0x94 };
static uint8_t APP_EUI[] = { 0x70, 0xB3, 0xD5, 0x7E, 0xD0, 0x00, 0xC1, 0x84 };
static uint8_t APP_KEY[] = { 0xB8, 0xB4, 0x33, 0x0D, 0xFD, 0xD5, 0xD8, 0x61, 0xE7, 0x37, 0xA6, 0xC9, 0x5E, 0x5F, 0xD3, 0xF0 };

static void lora_event_handler(lorawan_event_t event);
static void lora_uc_send(LoRaWANUpdateClientSendParams_t &params);
static void queue_next_send_message();
static void send_message();

static LoRaWANInterface lorawan(radio);
static ClockSyncControlPackage clk_sync_plugin;
static MulticastControlPackage mcast_plugin;
static FragmentationControlPackage frag_plugin;
static lorawan_app_callbacks_t callbacks;
static mcast_controller_cbs_t mcast_cbs;

static LoRaWANUpdateClient uc(&bd, APP_KEY, lora_uc_send);
static bool in_class_c_mode = false;
static LoRaWANUpdateClientSendParams_t queued_message;
static bool queued_message_waiting = false;

static DigitalOut led1(ACTIVITY_LED);

static void turn_led_on() {
    led1 = 1;
}
static void turn_led_off() {
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

static void switch_class(uint32_t device_class,
                         uint32_t time_to_switch,
                         uint8_t life_time,
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

static void lorawan_uc_fragsession_complete() {
    printf("Frag session is complete\n");
}

#if MBED_CONF_LORAWAN_UPDATE_CLIENT_INTEROP_TESTING
uint32_t interop_crc32 = 0x0;
static void lorawan_uc_firmware_ready(uint32_t crc) {
    uc.printHeapStats("FWREADY ");
    printf("Firmware is ready, CRC32 hash is %08lx\n", crc);
    interop_crc32 = crc;
}
#else
static void lorawan_uc_firmware_ready() {
    uc.printHeapStats("FWREADY ");
    printf("Firmware is ready, hit **RESET** to flash the firmware\n");

    // reboot system
    NVIC_SystemReset();
}
#endif

static void lora_uc_send(clk_sync_response_t *resp) {
    queued_message.port = CLOCK_SYNC_PORT;
    queued_message.length = resp->size;
    queued_message.confirmed = false; // @todo
    queued_message.retriesAllowed = resp->forced_resync_required;

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
    printf("Frag session resp length %lu\n", resp->size);
    for (size_t ix = 0; ix < resp->size; ix++) {
        printf("%02x ", resp->data[ix]);
    }
    printf("\n");

    queued_message.port = FRAGMENTATION_CONTROL_PORT;
    queued_message.length = resp->size;
    queued_message.confirmed = false; // @todo
    queued_message.retriesAllowed = true;

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

static lorawan_status_t check_params_validity(uint8_t dr, uint32_t dl_freq) {
    return lorawan.verify_multicast_freq_and_dr(dl_freq, dr);
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

    // Enable trace output for this demo, so we can see what the LoRaWAN stack does
    mbed_trace_init();
    mbed_trace_exclude_filters_set("QSPIF");

    clk_sync_plugin.activate_clock_sync_package(callback(&lorawan, &LoRaWANInterface::get_current_gps_time),
        callback(&lorawan, &LoRaWANInterface::set_current_gps_time));

    // Activate multicast plugin with the GenAppKey
    mcast_plugin.activate_multicast_control_package(APP_KEY, 16);

    // Multicast control package callbacks
    mcast_cbs.switch_class = callback(&switch_class);
    mcast_cbs.check_params_validity = callback(&check_params_validity);
    mcast_cbs.get_gps_time = callback(&lorawan, &LoRaWANInterface::get_current_gps_time);

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

frag_bd_opts_t *bd_cb_handler(uint8_t frag_index, uint32_t desc) {
    // @todo: actually should be able to handle multiple frag_index'es but not now
    static frag_bd_opts_t bd_opts;
    static FragAssembler assembler;
    static FragBDWrapper bdWrapper(&bd);

    uc.printHeapStats("BDCB-BEFORE ");

    printf("Creating storage layer for session %d with desc: %lu\n", frag_index, desc);

    bd_opts.redundancy_max = MBED_CONF_LORAWAN_UPDATE_CLIENT_MAX_REDUNDANCY;
    bd_opts.offset = MBED_CONF_LORAWAN_UPDATE_CLIENT_SLOT0_FW_ADDRESS;
    bd_opts.fasm = &assembler;
    bd_opts.bd = &bdWrapper;

    uc.printHeapStats("BDCB-AFTER ");

    return &bd_opts;
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
    for (size_t ix = 0; ix < retcode; ix++) {
        printf("%02x ", rx_buffer[ix]);
    }
    printf("\n");

    if (port == CLOCK_SYNC_PORT) {
        clk_sync_response_t *sync_resp = clk_sync_plugin.parse(rx_buffer, retcode);

        printf("Clock sync bla %p\n", sync_resp);

        if (sync_resp) {
            printf("Queuing some crap\n");
            lora_uc_send(sync_resp);
        }
    }
    else if (port == MULTICAST_CONTROL_PORT) {
        mcast_ctrl_response_t *mcast_resp = mcast_plugin.parse(rx_buffer, retcode,
                                        lorawan.get_multicast_addr_register(),
                                        &mcast_cbs);

        if (mcast_resp) {
            lora_uc_send(mcast_resp);
        }
    }
    else if (port == FRAGMENTATION_CONTROL_PORT) {
        lorawan_rx_metadata md;
        lorawan.get_rx_metadata(md);

        // blink LED when receiving a packet in Class C mode
        if (in_class_c_mode) {
            turn_led_on();
            evqueue.call_in(200, &turn_led_off);
        }

        frag_ctrl_response_t *frag_resp = frag_plugin.parse(rx_buffer, retcode, flags, md.dev_addr,
                                            mbed::callback(bd_cb_handler),
                                            lorawan.get_multicast_addr_register());

        if (frag_resp != NULL && frag_resp->type == FRAG_CMD_RESP) {
            lora_uc_send(&frag_resp->cmd_ans);
        }

        if (frag_resp != NULL && frag_resp->type == FRAG_SESSION_STATUS && frag_resp->status == FRAG_SESSION_COMPLETE) {
            lorawan_uc_fragsession_complete();
        }
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
            printf("Received message from Network Server\n");
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
