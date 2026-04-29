# Cleanup Game Demo

## Voice Prompt

Say:

```text
Hey Willow, time to cleanup.
```

Expected behavior:

- Willow starts a 10 minute cleanup game.
- The Echo Pyramid display shows `CLEANUP`.
- The RGB sliders switch into the cleanup dance pattern.
- Spotify starts cleanup music on the Hermes Echo Pyramid.
- Willow gives one short cleanup challenge.
- The device shows checkpoint prompts near the end and pauses music when done.

Useful variants:

```text
Hey Willow, start a five minute kitchen cleanup.
Hey Willow, how much cleanup time is left?
Hey Willow, stop cleanup.
```

## Fallback Checks

Gateway health:

```bash
curl http://127.0.0.1:8787/health
```

Device state:

```bash
curl -H 'X-Admin-Key: dev-admin-key' \
  http://127.0.0.1:8787/v1/devices/echo-pyramid-dev
```

Spotify speaker state:

```bash
curl -H 'X-Admin-Key: dev-admin-key' \
  http://127.0.0.1:8787/v1/music/librespot
```

## Notes

- The device intentionally shows disconnected until local WakeNet opens a WebRTC session.
- Cleanup music needs the device session connected because librespot pipes audio into the WebRTC output track.
- If Spotify starts but the device does not hear follow-up commands, say `hey Willow` first; music mode keeps Deepgram disconnected until local wake fires.
