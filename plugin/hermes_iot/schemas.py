IOT_GET_DEVICE_CONTEXT = {
    "type": "function",
    "name": "iot_get_device_context",
    "description": "Get connection and capability metadata for a paired Hermes IoT device.",
    "parameters": {
        "type": "object",
        "properties": {
            "device_id": {
                "type": "string",
                "description": "The target device ID, such as echo-pyramid-dev.",
            }
        },
        "required": ["device_id"],
        "additionalProperties": False,
    },
}

IOT_SET_LED = {
    "type": "function",
    "name": "iot_set_led",
    "description": "Set the target device LED state through the Hermes IoT gateway.",
    "parameters": {
        "type": "object",
        "properties": {
            "device_id": {"type": "string"},
            "color": {"type": "string", "description": "Named color or RGB hex string."},
            "pattern": {"type": "string", "description": "Simple effect such as solid, pulse, or breathe."},
        },
        "required": ["device_id", "color"],
        "additionalProperties": False,
    },
}

IOT_BEEP = {
    "type": "function",
    "name": "iot_beep",
    "description": "Play a short confirmation beep on the target device.",
    "parameters": {
        "type": "object",
        "properties": {
            "device_id": {"type": "string"},
            "frequency_hz": {"type": "integer", "minimum": 100, "maximum": 10000},
            "duration_ms": {"type": "integer", "minimum": 10, "maximum": 5000},
        },
        "required": ["device_id"],
        "additionalProperties": False,
    },
}

IOT_GET_TIME = {
    "type": "function",
    "name": "iot_get_time",
    "description": "Get the current local time for quick spoken answers.",
    "parameters": {
        "type": "object",
        "properties": {
            "timezone": {
                "type": "string",
                "description": "Optional IANA timezone such as America/Denver. Defaults to the server local timezone.",
            }
        },
        "required": [],
        "additionalProperties": False,
    },
}

IOT_SET_TIMER = {
    "type": "function",
    "name": "iot_set_timer",
    "description": "Set a named household timer and optionally notify a Hermes IoT device when it finishes.",
    "parameters": {
        "type": "object",
        "properties": {
            "label": {"type": "string", "description": "Short timer label, such as bread or laundry."},
            "duration_seconds": {"type": "integer", "minimum": 1, "maximum": 86400},
            "device_id": {
                "type": "string",
                "description": "Optional target IoT device ID for completion feedback.",
            },
        },
        "required": ["label", "duration_seconds"],
        "additionalProperties": False,
    },
}

IOT_CLEANUP_GAME = {
    "type": "function",
    "name": "iot_cleanup_game",
    "description": "Start a gamified household cleanup session with music, Echo Pyramid lights, display prompts, and an automatic finish signal.",
    "parameters": {
        "type": "object",
        "properties": {
            "duration_minutes": {
                "type": "integer",
                "minimum": 1,
                "maximum": 30,
                "description": "Cleanup duration in minutes. Defaults to 10.",
            },
            "device_id": {
                "type": "string",
                "description": "Optional target IoT device ID. Defaults to the current configured device.",
            },
            "label": {"type": "string", "description": "Short cleanup label, such as kitchen or playroom."},
            "music_query": {
                "type": "string",
                "description": "Optional Spotify search query for the cleanup music.",
            },
        },
        "required": [],
        "additionalProperties": False,
    },
}

IOT_CLEANUP_STATUS = {
    "type": "function",
    "name": "iot_cleanup_status",
    "description": "Check running gamified cleanup sessions.",
    "parameters": {
        "type": "object",
        "properties": {"id": {"type": "string", "description": "Optional cleanup session ID."}},
        "required": [],
        "additionalProperties": False,
    },
}

IOT_CLEANUP_STOP = {
    "type": "function",
    "name": "iot_cleanup_stop",
    "description": "Stop running gamified cleanup sessions and pause cleanup music.",
    "parameters": {
        "type": "object",
        "properties": {"id": {"type": "string", "description": "Optional cleanup session ID."}},
        "required": [],
        "additionalProperties": False,
    },
}

IOT_END_CONVERSATION = {
    "type": "function",
    "name": "iot_end_conversation",
    "description": "End the current IoT voice conversation after the final spoken reply, closing the live audio session so the device returns to wake-word standby.",
    "parameters": {
        "type": "object",
        "properties": {
            "device_id": {
                "type": "string",
                "description": "Optional target IoT device ID. Defaults to the current configured device.",
            }
        },
        "required": [],
        "additionalProperties": False,
    },
}

IOT_MEDIA_STATUS = {
    "type": "function",
    "name": "iot_media_status",
    "description": "Get Spotify Connect playback service status for the Hermes IoT speaker.",
    "parameters": {
        "type": "object",
        "properties": {},
        "required": [],
        "additionalProperties": False,
    },
}

IOT_MEDIA_SEARCH = {
    "type": "function",
    "name": "iot_media_search",
    "description": "Search Spotify for a track, album, or playlist by name.",
    "parameters": {
        "type": "object",
        "properties": {
            "query": {"type": "string", "description": "The song, album, artist, or playlist search query."},
            "media_type": {
                "type": "string",
                "enum": ["track", "album", "playlist"],
                "description": "Spotify media type to search. Defaults to track.",
            },
            "limit": {"type": "integer", "minimum": 1, "maximum": 10},
        },
        "required": ["query"],
        "additionalProperties": False,
    },
}

IOT_MEDIA_PLAY = {
    "type": "function",
    "name": "iot_media_play",
    "description": "Start new Spotify playback on the Hermes IoT speaker by query or Spotify URI. Do not use this for skip, pause, stop, or resume commands.",
    "parameters": {
        "type": "object",
        "properties": {
            "query": {"type": "string", "description": "Natural-language song query, such as Shake It Off by Taylor Swift."},
            "uri": {"type": "string", "description": "Optional Spotify URI returned by search."},
        },
        "required": [],
        "additionalProperties": False,
    },
}

IOT_MEDIA_PAUSE = {
    "type": "function",
    "name": "iot_media_pause",
    "description": "Pause Spotify playback on the Hermes IoT speaker.",
    "parameters": {"type": "object", "properties": {}, "required": [], "additionalProperties": False},
}

IOT_MEDIA_RESUME = {
    "type": "function",
    "name": "iot_media_resume",
    "description": "Resume Spotify playback on the Hermes IoT speaker.",
    "parameters": {"type": "object", "properties": {}, "required": [], "additionalProperties": False},
}

IOT_MEDIA_NEXT = {
    "type": "function",
    "name": "iot_media_next",
    "description": "Skip to the next Spotify track on the Hermes IoT speaker. Use this directly for skip, next song, or play the next song commands.",
    "parameters": {"type": "object", "properties": {}, "required": [], "additionalProperties": False},
}

IOT_MEDIA_VOLUME = {
    "type": "function",
    "name": "iot_media_volume",
    "description": "Set Spotify playback volume on the Hermes IoT speaker.",
    "parameters": {
        "type": "object",
        "properties": {
            "percent": {"type": "integer", "minimum": 0, "maximum": 100},
        },
        "required": ["percent"],
        "additionalProperties": False,
    },
}

IOT_MEDIA_START = {
    "type": "function",
    "name": "iot_media_start",
    "description": "Start the Hermes IoT Spotify Connect service so Spotify can play to the speaker.",
    "parameters": {
        "type": "object",
        "properties": {},
        "required": [],
        "additionalProperties": False,
    },
}

IOT_MEDIA_STOP = {
    "type": "function",
    "name": "iot_media_stop",
    "description": "Stop Spotify Connect playback on the Hermes IoT speaker.",
    "parameters": {
        "type": "object",
        "properties": {},
        "required": [],
        "additionalProperties": False,
    },
}
