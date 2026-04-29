# Echo Pyramid firmware

This firmware targets the M5Stack Echo Pyramid with an AtomS3/S3R controller using ESP-IDF.

## Current state

- Real Echo Pyramid audio bring-up is working on hardware.
- ES8311 and ES7210 initialize successfully.
- Local speaker self-test runs at reduced nighttime volume.
- Boot now degrades cleanly when Wi-Fi provisioning is unset.
- Menuconfig-backed Wi-Fi and gateway bootstrap hooks are in place.
- WebRTC signaling/media are not wired yet.

## Verified hardware loop

The following flow has been verified on `/dev/cu.usbmodem101`:

1. `idf.py build`
2. `idf.py -p /dev/cu.usbmodem101 flash monitor`
3. Board boots and detects live Echo Pyramid I2C peripherals
4. Audio codecs initialize
5. Local self-test tone plays
6. Status reaches `READY`
7. If Wi-Fi is unset, status settles on `READY (Needs WiFi)`
8. Claimed bootstrap data is persisted in NVS for later reuse

## Configuration

These values are set through `idf.py menuconfig` under the Hermes IoT section:

- `CONFIG_HERMES_IOT_WIFI_SSID`
- `CONFIG_HERMES_IOT_WIFI_PASSWORD`
- `CONFIG_HERMES_IOT_GATEWAY_BASE_URL`
- `CONFIG_HERMES_IOT_DEVICE_ID`
- `CONFIG_HERMES_IOT_FIRMWARE_VERSION`

Until Wi-Fi is configured, the firmware will skip gateway bootstrap and remain usable for local bring-up.

## Runtime split

- `board_audio.cpp`: codec, amp, I2C, I2S, and local tone playback
- `board_status.cpp`: local state/status reporting
- `wifi_manager.cpp`: station-mode Wi-Fi bring-up
- `gateway_client.cpp`: `/health` and `/v1/pair/claim` bootstrap calls
- `app_main.cpp`: boot orchestration and state transitions

## Next firmware steps

1. Add touch, mute, and RGB state handling on real hardware.
2. Reuse cached bootstrap state during signaling startup.
3. Wire WebRTC signaling and data channel bootstrap.
4. Connect mic capture and speaker playback to the WebRTC media path.
