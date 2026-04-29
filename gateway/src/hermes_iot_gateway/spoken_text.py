from __future__ import annotations

import re


IOT_VOICE_SYSTEM_PROMPT = (
    "You are speaking out loud through a small Hermes IoT voice device. "
    "Write only plain spoken text for text-to-speech. Do not use Markdown, "
    "asterisk emphasis, headings, bullets, numbered lists, code fences, tables, "
    "or links. Keep formatting invisible because punctuation such as ** will be "
    "spoken aloud by the voice."
)


_FENCED_CODE_RE = re.compile(r"```(?:[^\n`]*)\n?(.*?)```", re.DOTALL)
_INLINE_CODE_RE = re.compile(r"`([^`]+)`")
_LINK_RE = re.compile(r"\[([^\]]+)\]\(([^)]+)\)")
_IMAGE_RE = re.compile(r"!\[([^\]]*)\]\(([^)]+)\)")
_BOLD_ITALIC_RE = re.compile(r"(?<!\w)(\*\*\*|___)(.+?)\1(?!\w)", re.DOTALL)
_BOLD_RE = re.compile(r"(?<!\w)(\*\*|__)(.+?)\1(?!\w)", re.DOTALL)
_ITALIC_RE = re.compile(r"(?<!\w)(\*|_)([^*_]+?)\1(?!\w)", re.DOTALL)
_HEADING_RE = re.compile(r"(?m)^\s{0,3}#{1,6}\s+")
_BLOCKQUOTE_RE = re.compile(r"(?m)^\s{0,3}>\s?")
_LIST_MARKER_RE = re.compile(r"(?m)^\s*(?:[-*+]\s+|\d+[.)]\s+)")
_RULE_RE = re.compile(r"(?m)^\s{0,3}(?:[-*_]\s*){3,}$")
_BARE_MARKUP_RE = re.compile(r"[*_~]{2,}")


def sanitize_spoken_text(text: str, *, strip: bool = True) -> str:
    """Remove lightweight Markdown that TTS would otherwise read literally."""

    if not text:
        return text

    cleaned = text
    cleaned = _FENCED_CODE_RE.sub(lambda match: match.group(1).strip(), cleaned)
    cleaned = _IMAGE_RE.sub(lambda match: match.group(1), cleaned)
    cleaned = _LINK_RE.sub(lambda match: match.group(1), cleaned)
    cleaned = _INLINE_CODE_RE.sub(lambda match: match.group(1), cleaned)
    cleaned = _BOLD_ITALIC_RE.sub(lambda match: match.group(2), cleaned)
    cleaned = _BOLD_RE.sub(lambda match: match.group(2), cleaned)
    cleaned = _ITALIC_RE.sub(lambda match: match.group(2), cleaned)
    cleaned = _HEADING_RE.sub("", cleaned)
    cleaned = _BLOCKQUOTE_RE.sub("", cleaned)
    cleaned = _LIST_MARKER_RE.sub("", cleaned)
    cleaned = _RULE_RE.sub("", cleaned)
    cleaned = _BARE_MARKUP_RE.sub("", cleaned)
    cleaned = re.sub(r"\n{3,}", "\n\n", cleaned)
    return cleaned.strip() if strip else cleaned
