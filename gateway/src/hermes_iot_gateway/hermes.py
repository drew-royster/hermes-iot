from __future__ import annotations

import json
from collections.abc import AsyncIterator
from dataclasses import dataclass
from typing import Any

import httpx

from .config import Settings
from .models import HermesStreamEvent


@dataclass(slots=True)
class SSEFrame:
    event: str
    data: str


def _iter_sse_frames(lines: list[str]) -> list[SSEFrame]:
    frames: list[SSEFrame] = []
    event = "message"
    payload_lines: list[str] = []
    for line in lines + [""]:
        if not line:
            if payload_lines:
                frames.append(SSEFrame(event=event, data="\n".join(payload_lines)))
            event = "message"
            payload_lines = []
            continue
        if line.startswith("event:"):
            event = line.split(":", 1)[1].strip()
        elif line.startswith("data:"):
            payload_lines.append(line.split(":", 1)[1].lstrip())
    return frames


def normalize_responses_event(event: str, data: str) -> HermesStreamEvent | None:
    payload = json.loads(data)
    if event == "response.created":
        return HermesStreamEvent(kind="response.created", payload={"id": payload.get("response", {}).get("id")})
    if event == "response.output_text.delta":
        return HermesStreamEvent(kind="assistant.text.delta", payload={"text": payload.get("delta", "")})
    if event in {"response.output_item.added", "response.output_item.done"}:
        item = payload.get("item", {})
        item_type = item.get("type")
        if item_type == "function_call":
            return HermesStreamEvent(
                kind="tool.call",
                payload={
                    "name": item.get("name"),
                    "arguments": item.get("arguments"),
                    "call_id": item.get("call_id"),
                },
            )
        if item_type == "function_call_output":
            return HermesStreamEvent(
                kind="tool.output",
                payload={"call_id": item.get("call_id"), "output": item.get("output")},
            )
    if event == "response.completed":
        return HermesStreamEvent(kind="response.completed", payload={"response": payload.get("response", {})})
    if event == "response.failed":
        return HermesStreamEvent(kind="response.failed", payload=payload)
    return None


class HermesResponsesClient:
    def __init__(self, settings: Settings, http_client: httpx.AsyncClient | None = None) -> None:
        self._settings = settings
        self._client = http_client or httpx.AsyncClient(timeout=60)

    async def stream_text_turn(
        self,
        *,
        device_id: str,
        conversation: str,
        text: str,
        hello_metadata: dict[str, Any] | None = None,
    ) -> AsyncIterator[HermesStreamEvent]:
        instructions = self._settings.default_instructions
        if hello_metadata:
            instructions = (
                f"{instructions}\n\n"
                f"Device metadata:\n{json.dumps(hello_metadata, indent=2, sort_keys=True)}"
            )
        payload = {
            "model": self._settings.hermes_model,
            "conversation": conversation,
            "store": True,
            "stream": True,
            "input": text,
            "instructions": instructions,
        }
        headers = {"Authorization": f"Bearer {self._settings.hermes_api_key}"}
        url = f"{self._settings.hermes_api_base_url}/responses"

        async with self._client.stream("POST", url, headers=headers, json=payload) as response:
            response.raise_for_status()
            buffered_lines: list[str] = []
            async for line in response.aiter_lines():
                buffered_lines.append(line)
                if line == "":
                    for frame in _iter_sse_frames(buffered_lines):
                        normalized = normalize_responses_event(frame.event, frame.data)
                        if normalized:
                            yield normalized
                    buffered_lines = []
