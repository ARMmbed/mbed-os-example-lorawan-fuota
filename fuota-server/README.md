# FUOTA Server

Simple test FUOTA server for loraserver.io. To be used together with [this device software](https://github.com/armmbed/mbed-os-example-lorawan-fuota). Based on FUOTA test scenarios document.

## Setup

1. Install loraserver and all dependencies.
1. Add your devices to loraserver with parameters:
    * LoRaWAN 1.0.2
    * OTAA activation
    * App Key: `000102030405060708090A0B0C0D0E0F`
1. Add a single extra device (this will be used as multicast group), with parameters:
    * LoRaWAN 1.0.2
    * ABP activation
    * Class C
    * DevAddr: `0x01FFFFFF`
    * NwkSKey: `BB75C362588F5D65FCC61C080B76DBA3`
    * AppSKey: `C3F6C39B6B6496C29629F7E7E9B0CD29`
1. To establish a connection between this device and the gateway make sure to send at least one message from the Class C device to the network (can also be done in the simulator). If you're on L-TEK FF1705 or Multi-Tech xDot and EU868 you can do this by flashing [xdot-classc-mc-activation-eu868.bin](xdot-classc-mc-activation-eu868.bin) to your device, clicking **RESET** and pressing **BUTTON1**. Observe the 'Live LoRaWAN frame logs' to verify that the message appeared.
1. In `loraserver.js`:
    * Set the IP address of your server under `LORASERVER_HOST`.
    * Add your device EUIs from step 2 to the `devices` array.
    * Add the device EUI from step 3 to the `mcDetails` object.
1. Run:

    ```
    $ node loraserver.js PATH_TO_A_PACKETS_FILE
    ```

1. Restart your devices to re-trigger a clock sync.

Once all devices have done a clock sync, a multicast session will automatically start.

## Switching to a higher spreading factor

LoRaServer has no notion of multicast, thus always sends out Class C packets on the RX2 data rate and frequency. This will be very slow in most regions (e.g. EU868). You can however overwrite this with some changes. This is how to use SF7 in EU868.

1. Log in to the LoRaServer server, and open `/etc/loraserver/loraserver.toml`.
1. Set:

    ```
    rx2_dr=5
    ```

1. Restart the server:

    ```
    $ sudo systemctl restart lora-app-server
    $ sudo systemctl restart lora-gateway-bridge
    $ sudo systemctl restart loraserver
    ```

1. In `loraserver.js` under `DATARATE`, set to `5`.
1. Change the sleep time under `startSendingClassCPackets` to send faster.
1. Afterwards, send at least one message from the Class C device again.

**Note:** This will not allow you to receive any messages in RX2 window on your Class A sessions, so use with care.
