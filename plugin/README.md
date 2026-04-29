# Hermes IoT Plugin

This plugin adds the Hermes-side extension layer for the IoT gateway:

- device context injection via `pre_llm_call`
- low-risk home/device tools such as `iot_get_time`, `iot_set_timer`, `iot_set_led`, and `iot_beep`
- a device-session skill for spoken/device-aware interactions

## Install into Hermes

For the full Hermes-home install flow:

```bash
source .venv/bin/activate
hermes-iot-setup --force
```

If you only want the plugin assets and not the gateway config:

```bash
source .venv/bin/activate
hermes-iot-install-plugin
```

This copies the plugin into `~/.hermes/plugins/hermes_iot` for the active Hermes profile layout.
