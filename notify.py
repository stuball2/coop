import logging
import os

import requests

logger = logging.getLogger(__name__)


def send_telegram(message: str) -> None:
    """Send a Telegram message. Silently no-ops if credentials are not configured."""
    token = os.environ.get("TELEGRAM_BOT_TOKEN")
    chat_id = os.environ.get("TELEGRAM_CHAT_ID")
    if not token or not chat_id:
        return
    try:
        resp = requests.post(
            f"https://api.telegram.org/bot{token}/sendMessage",
            json={"chat_id": chat_id, "text": message},
            timeout=5,
        )
        resp.raise_for_status()
    except Exception as exc:
        # Notifications are best-effort — never let this crash the main request
        logger.warning("Telegram notification failed: %s", exc)
