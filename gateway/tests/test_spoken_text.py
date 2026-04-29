from hermes_iot_gateway.spoken_text import IOT_VOICE_SYSTEM_PROMPT, sanitize_spoken_text


def test_sanitize_spoken_text_removes_markdown_emphasis() -> None:
    text = 'Sure -- if you meant **Poe** or **"Pokin"**, tell me.'

    assert sanitize_spoken_text(text) == 'Sure -- if you meant Poe or "Pokin", tell me.'


def test_sanitize_spoken_text_preserves_delta_spacing_when_requested() -> None:
    assert sanitize_spoken_text("Hello ", strip=False) == "Hello "


def test_iot_voice_prompt_bans_markdown() -> None:
    assert "Do not use Markdown" in IOT_VOICE_SYSTEM_PROMPT
    assert "spoken aloud" in IOT_VOICE_SYSTEM_PROMPT
