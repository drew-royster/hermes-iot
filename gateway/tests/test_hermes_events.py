from hermes_iot_gateway.hermes import _iter_sse_frames, normalize_responses_event


def test_iter_sse_frames_splits_event_blocks() -> None:
    frames = _iter_sse_frames(
        [
            "event: response.output_text.delta",
            'data: {"delta":"Hello"}',
            "",
            "event: response.completed",
            'data: {"response":{"id":"resp_123"}}',
            "",
        ]
    )
    assert len(frames) == 2
    assert frames[0].event == "response.output_text.delta"
    assert frames[1].event == "response.completed"


def test_normalize_function_call_item() -> None:
    event = normalize_responses_event(
        "response.output_item.added",
        '{"item":{"type":"function_call","name":"iot_set_led","arguments":"{}","call_id":"call_1"}}',
    )
    assert event is not None
    assert event.kind == "tool.call"
    assert event.payload["name"] == "iot_set_led"

