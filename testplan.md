# Test plan

Please run the following tests before a release.

- Unit tests - OK

    ```
    mbed test --app-config mbed-lorawan-update-client/TESTS/tests/mbed_app.json -n mbed-lorawan-update-client-tests-tests-* -v
    ```

- Interop test

    ```
    # in mbed_app.json set
    # "lorawan-update-client.interop-testing"     : true,

    $ cp .mbedignore_no_rtos .mbedignore
    $ mbed compile --profile=./profiles/tiny.json
    $ cp BUILD/FF1705_L151CC/GCC_ARM-TINY/mbed-os-example-lorawan-fuota.bin /Volumes/FF1705
    $ lorawan-fota-signing-tool create-frag-packets -i fuota-server/test-file.bin --output-format plain --frag-size 40 --redundancy-packets 5 -o fuota-server/test-file-unsigned.txt
    $ LORA_HOST=192.168.122.134 LORA_DR=5 node fuota-server/loraserver.js fuota-server/test-file-unsigned.txt
    ```

- Blinky

    ```
    # in mbed_app.json set
    # "lorawan-update-client.interop-testing"     : false,

    $ mbed compile --profile=./profiles/tiny.json
    $ cp BUILD/FF1705_L151CC/GCC_ARM-TINY/mbed-os-example-lorawan-fuota.bin /Volumes/FF1705
    $ lorawan-fota-signing-tool sign-binary -b example-firmware/xdot-blinky.bin -o fuota-server/xdot-blinky-signed.txt --frag-size 204 --redundancy-packets 20 --output-format packets-plain --override-version
    $ LORA_HOST=192.168.122.134 LORA_DR=5 node fuota-server/loraserver.js fuota-server/xdot-blinky-signed.txt
    ```

- Delta update

    ```
    $ rm updates/*
    $ mbed compile --profile=./profiles/tiny.json
    $ cp BUILD/FF1705_L151CC/GCC_ARM-TINY/mbed-os-example-lorawan-fuota.bin /Volumes/FF1705
    $ cp BUILD/FF1705_L151CC/GCC_ARM-TINY/mbed-os-example-lorawan-fuota_application.bin updates/v1.bin

    # make change in app

    $ mbed compile --profile=./profiles/tiny.json
    $ cp BUILD/FF1705_L151CC/GCC_ARM-TINY/mbed-os-example-lorawan-fuota_application.bin updates/v2.bin
    $ lorawan-fota-signing-tool sign-delta --old updates/v1.bin --new updates/v2.bin --output-format packets-plain -o updates/v1_to_v2.txt --frag-size 204 --redundancy-packets 10
    $ LORA_HOST=192.168.122.132 LORA_DR=5 node fuota-server/loraserver.js updates/v1_to_v2.txt
    ```
