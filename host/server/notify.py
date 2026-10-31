from __future__ import annotations
import json
from urllib.request import Request, urlopen

def publish_ntfy(base_url: str, topic: str, title: str, message: str, token: str, timeout: float = 5.0) -> str:
    if not base_url.startswith("https://") or not topic or len(message) > 4096: return "REJECTED"
    # ntfy's JSON publish endpoint is the server root. Posting the same JSON to
    # /<topic> wraps it as a literal message and loses title/tag semantics.
    req=Request(base_url.rstrip("/"),data=json.dumps({"topic":topic,"title":title,"message":message,"tags":["warning"]}).encode(),headers={"Authorization":"Bearer "+token,"Content-Type":"application/json"},method="POST")
    try:
        with urlopen(req, timeout=timeout) as response: return "CHANNEL_ACCEPTED" if 200 <= response.status < 300 else "CHANNEL_FAILED"
    except Exception: return "DELIVERY_UNKNOWN"
