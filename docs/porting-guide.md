# Porting guide

This application runs on every Mbed-enabled development board, but some configuration needs to be set.

1. Storage layer needs to be specified in both the [bootloader](https://github.com/janjongboom/mbed-bootloader/tree/ff1705_l151cc) and in this application (both in `main.cpp`).
1. Firmware slots need to be configured in both the bootloader and this application.
1. A Root of Trust needs to be configured. An example insecure RoT is provided, but you **should** replace this before going into production. See the [Pelion Device Management docs](https://cloud.mbed.com/docs/v1.4/connecting/pelion-device-management-edge-security-considerations.html#root-of-trust) for more information.

## Adding a new section in mbed_app.json

The [mbed_app.json](../mbed-app.json) file contains all the configuration for the application, and this also contains per-target configuration. To start a new port you'll need to add a new section for your board under `target_overrides`. The key of the configuration is the name of your target. You can find this name through Mbed CLI:

```
$ mbed detect
[mbed] Detected DISCO_L475VG_IOT01A, port /dev/tty.usbmodem14403, mounted /Volumes/DIS_L4IOT, interface version 0221:
[mbed] Supported toolchains for DISCO_L475VG_IOT01A
| Target              | mbed OS 2 | mbed OS 5 |    uARM   |    IAR    |    ARM    |  GCC_ARM  |
|---------------------|-----------|-----------|-----------|-----------|-----------|-----------|
| DISCO_L475VG_IOT01A | Supported | Supported | Supported | Supported | Supported | Supported |
Supported targets: 1
Supported toolchains: 4
```

Here `DISCO_L475VG_IOT01A` is the target name.

Thus, add the following section:

```json
        "DISCO_L475VG_IOT01A": {
            "target.features_add"                       : ["BOOTLOADER"]
        }
```

All configuration options in this porting guide need to be added to this section.

## Activity LED

An activity LED is used when fragments are received. This is required by the LoRa Alliance interop tests. By default `LED1` is used, but you can override it. For example, ST boards use the same pin for `LED1` and for pins used to communicate to the radio. You can also disable the LED, by setting the value to `NC` (Not Connected).

The LED is set through:

```json
            "activity-led":        "D3",
```

## LoRa radio

You need to tell the application how the radio is connected to your development board over the SPI interface. For the [SX1272](https://os.mbed.com/components/SX1272MB2xAS/) and [SX1276](https://os.mbed.com/components/SX1276MB1xAS/) shields the configuration is:

```json
            "lora-radio":          "SX1276",
            "lora-spi-mosi":       "D11",
            "lora-spi-miso":       "D12",
            "lora-spi-sclk":       "D13",
            "lora-cs":             "D10",
            "lora-reset":          "A0",
            "lora-dio0":           "D2",
            "lora-dio1":           "D3",
            "lora-dio2":           "D4",
            "lora-dio3":           "D5",
            "lora-dio4":           "D8",
            "lora-dio5":           "D9",
            "lora-rf-switch-ctl1": "NC",
            "lora-rf-switch-ctl2": "NC",
            "lora-txctl":          "NC",
            "lora-rxctl":          "NC",
            "lora-ant-switch":     "A4",
            "lora-pwr-amp-ctl":    "NC",
            "lora-tcxo":           "NC",
```

Set `lora-radio` to `SX1272` if you use the SX1272 shield.

## Storage layer

Mbed OS has a wide variety of storage drivers available, including SD cards, DataFlash, SPI flash, and QSPI flash. The drivers are in the [components/storage/blockdevice](https://github.com/ARMmbed/mbed-os/tree/master/components/storage/blockdevice) folder of Mbed OS. You can use any of these storage layers to store the firmware fragments. If you have a lot of internal flash you can also use the [FlashIAP](https://github.com/ARMmbed/mbed-os/tree/master/components/storage/blockdevice/COMPONENT_FLASHIAP) driver.

To select a built-in driver, add:

```json
            "target.components_add"                     : ["QSPIF"],
```

Note that you can write your own storage drivers, as long as they implement the `BlockDevice` interface. In that case you don't need to do add the component in configuration, just add the driver to the project.

### Selecting the storage layer

To instantiate the storage layer, open [storage_helper.h](../source/helpers/storage_helper.h), and add a section for your board where you innstantiate the block device. Make sure to name it `bd`.

```cpp
if defined(TARGET_DISCO_L475VG_IOT01A)
// QSPI Flash interface
#include "QSPIFBlockDevice.h"
QSPIFBlockDevice bd(QSPI_FLASH1_IO0, QSPI_FLASH1_IO1, QSPI_FLASH1_IO2, QSPI_FLASH1_IO3, QSPI_FLASH1_SCK, QSPI_FLASH1_CSN, QSPIF_POLARITY_MODE_0, MBED_CONF_QSPIF_QSPI_FREQ);
#endif
```

### Slicing block devices

The update client assumes it has full access to the storage layer. If you want to slice the storage in multiple parts, for example to mount a file system on part of the block device, use [SlicingBlockDevice](https://os.mbed.com/docs/mbed-os/v5.11/apis/slicingblockdevice.html).

## Configuring firmware slots

The update client requires three firmware slots:

* Slot 0: to download incoming firmware.
* Slot 1: to store the results of a delta update.
* Slot 2: to store the current active firmware.

In a full update only slot 0 is used. In a delta update slot 0 and slot 2 are combined and the result is stored in slot 1. During boot the bootloader checks these slots to see if a new update is present.

You need to configure the location and sizes of these slots in the configuration file. For every slot we need two locations: the beginning of the slot (where the slot header is placed), and the beginning of the firmware (after the slot header). The exact location depends on:

* Maximum size of firmware. Typically this is the size of internal flash.
* Sector alignment of the external flash.

Let's say we have a device with:

* 512K internal flash.
* 4096 bytes erase sector size.
* 1 byte program sector size.
* 1 byte read sector size.

We can lay out the three blocks at the beginning of flash. Every slot needs to be erase sector aligned, and you want to start at the second sector (due to a bug in the bootloader). We get the following locations:

* Slot size: `512 * 1024` (max size of the firmware).
* Slot 0:
    * header-address: `4096` - needs to be erase sector aligned, and skipping the first sector.
    * fw-address: `4096 + 296` - the header is 296 bytes, and the fw-address needs to be program sector aligned to the next sector. For this device the sector size is 1 byte, so it's already aligned.
* Slot 1:
    * header-address: `4096 + (512 * 1024)`
    * fw-address: `4096 + (512 * 1024) + 296`
* Slot 2:
    * header-address: `4096 + (2 * 512 * 1024)`
    * fw-address: `4096 + (2 * 512 * 1024) + 72` - the slot 2 header is 72 bytes, and does **not** need to be program sector aligned. So this value is _always_ 72.

You place these values in your configuration as such:

```
            "lorawan-update-client.slot-size"           : "512 * 1024",
            "lorawan-update-client.slot0-header-address": "4096",
            "lorawan-update-client.slot0-fw-address"    : "4096 + 296",
            "lorawan-update-client.slot1-header-address": "4096 + (512 * 1024)",
            "lorawan-update-client.slot1-fw-address"    : "4096 + (512 * 1024) + 296",
            "lorawan-update-client.slot2-header-address": "4096 + (2 * 512 * 1024)",
            "lorawan-update-client.slot2-fw-address"    : "4096 + (2 * 512 * 1024) + 72",
```

## Bootloader

To actually perform a firmware update you need to have the bootloader present. The bootloader checks the external flash for new images and actually performs the update. The internal flash is laid out like this:

```
+--------------------------+
|                          |
|                          |
|                          |
|                          |
|                          |
|                          |
|       Application        |
|                          |
|                          |
|                          |
|                          |
+--------------------------+    APPLICATION (e.g. 0x8800)
|     Active fw header     |
|                          |
+--------------------------+    HEADER (e.g. 0x8000)
|                          |
|        Bootloader        |
|                          |
+--------------------------+    BEGIN (e.g. 0x0)
```

The typical bootloader size is 32K, and the active firmware header is under 1K. These locations however need to be erase sector aligned (on internal flash! it differs from the external flash alignment). You can get the alignment from your data sheet or through the `FlashIAP` API in Mbed OS. These addresses need to be configured.

In addition you need to configure the external flash driver (similar to your main application), and configure the firmware slot locations.

To create a new bootloader for your target:

1. Clone the bootloader source code from here: [mbed-bootloader](https://github.com/janjongboom/mbed-bootloader/tree/ff1705_l151cc).
1. Create a new section in `mbed_app.json` for your target.
1. Add the storage layer in the same way as above. Loading the driver is done in `main.cpp`.

Then add the following options to the `mbed_app.json` section:

```json
            "flash-start-address"              : "0x08000000",
            "flash-size"                       : "(512*1024)",
            "update-client.application-details": "(MBED_CONF_APP_FLASH_START_ADDRESS+32*1024)",
            "application-start-address"        : "(MBED_CONF_APP_FLASH_START_ADDRESS+34*1024)",
            "update-client.storage-address"    : "4096",
            "update-client.storage-size"       : "512 * 1024 * 2",
            "update-client.storage-page"       : "4096",
            "update-client.storage-locations"  : 2,
            "external-flash-application-copy-address": "4096 + (2 * 512 * 1024)",
            "max-application-size"             : "DEFAULT_MAX_APPLICATION_SIZE",
            "target.macros_add"                : [
                "MBED_CLOUD_CLIENT_UPDATE_BUFFER=0x2000",
                "BUFFER_SIZE=0x4000"
            ]
```

These values are:

| Key  | Description |
| ------------- | ------------- |
| flash-start-address  | Location where your memory-mapped flash starts. Look in the data sheet for your device to find this. E.g. `0x08000000` for STM32. |
| flash-size  | Size of the internal flash for your device.  |
| update-client.application-details | Location of the active firmware header. This needs to be after the bootloader. So look at the size of the bootloader, and place it after. 32K is pretty safe. This needs to be erase-sector aligned on the internal flash. You can use the `FlashIAP` API in Mbed OS to find alignment. |
| application-start-address | Location where the application starts. This is after the bootloader and the firmware header. This needs to be erase-sector aligned on the internal flash. |
| update-client.storage-address | Offset in external storage where the firmware slots start. Same value as `lorawan-update-client.slot0-header-address` in your application. |
| update-client.storage-size | *Total* size of the first two firmware slots. Not all three because slot 2 contains the active firmware, and we never want to flash from that. |
| update-client.storage-page | Erase sector size on the external flash. |
| update-client.storage-locations | Always 2 for this application. |
| external-flash-application-copy-address | Start address of slot 2. Same as `lorawan-update-client.slot2-header-address` in the application. |
| MBED_CLOUD_CLIENT_UPDATE_BUFFER | Needs to be twice `update-client.storage-page`. |
| BUFFER_SIZE | Scratch buffer, needs to be a multiple of `update-client.storage-page`. |

After this, build your bootloader via:

```
$ mbed compile --profile=./tiny.json
```

Copy the output file to the `bootloader` folder in this project.

### Loading the bootloader

Go back to `mbed_app.json` of your main project, and specify the bootloader via:

```json
            "target.header_offset"                      : "0x8000",
            "target.app_offset"                         : "0x8800",
            "target.bootloader_img"                     : "bootloader/MY_BOOTLOADER.bin",
```

Make sure that `header_offset` and `app_offset` match the values in the bootloader config for `update-client.application-details` and `application-start-address`, but offset to the start of the flash.
