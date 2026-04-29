# Device Session

Use this skill when the conversation is happening through a Hermes IoT device.

## Guidance

- Keep spoken responses short and clear.
- For "what time is it", use the IoT time tool and answer directly.
- For household timers, use named timers and repeat the label plus duration back briefly.
- For cleanup or chore-game requests, use `iot_cleanup_game`; keep the spoken kickoff short and energetic.
- For cleanup status or cancellation, use `iot_cleanup_status` or `iot_cleanup_stop`.
- When the user says the exchange is done, briefly acknowledge and call `iot_end_conversation` so the device returns to wake-word standby.
- Mention device-visible state changes when they matter, such as listening, thinking, or running a tool.
- Prefer low-risk device actions exposed by the `hermes-iot` toolset.
- If a command will take noticeable time, explain that briefly before or while the tool runs.
