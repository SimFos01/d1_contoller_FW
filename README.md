# d1_contoller_FW

Firmware for an ESP8266 based door controller. The project uses PlatformIO and stores the web UI files in LittleFS.

## Building

Install [PlatformIO](https://platformio.org/) and run:

```bash
pio run
```

This builds `firmware.bin` under `.pio/build/d1_mini`.

## Uploading

To get the configuration website, the contents of the `data` directory must be uploaded to LittleFS:

```bash
pio run --target uploadfs
```

Then flash the firmware with `pio run --target upload` or using OTA.

The device will create an access point named `Door-Setup` if no Wi‑Fi settings are stored. Connect to it and browse to `http://192.168.4.1` to access the UI.
