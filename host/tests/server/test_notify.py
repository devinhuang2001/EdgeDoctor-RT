from server.notify import publish_ntfy
import json
def test_rejects_insecure_or_oversized():
    assert publish_ntfy("http://localhost","x","t","m","secret")=="REJECTED"
    assert publish_ntfy("https://ntfy","x","t","x"*4097,"secret")=="REJECTED"

def test_json_publish_uses_server_root(monkeypatch):
    import server.notify as module
    captured = {}
    class Response:
        status = 200
        def __enter__(self): return self
        def __exit__(self, *args): return False
    def fake_open(request, timeout):
        captured["url"] = request.full_url
        captured["body"] = json.loads(request.data)
        return Response()
    monkeypatch.setattr(module, "urlopen", fake_open)
    assert publish_ntfy("https://ntfy.example/", "topic", "title", "body", "") == "CHANNEL_ACCEPTED"
    assert captured["url"] == "https://ntfy.example"
    assert captured["body"] == {"topic":"topic", "title":"title", "message":"body", "tags":["warning"]}

def test_notification_status_hides_credentials(tmp_path, monkeypatch):
    import server.app as module
    monkeypatch.setattr(module, "DB_PATH", tmp_path / "x.db")
    monkeypatch.setenv("EDGEDOCTOR_NTFY_URL", "https://ntfy.sh")
    monkeypatch.setenv("EDGEDOCTOR_NTFY_TOPIC", "private-topic")
    monkeypatch.setenv("EDGEDOCTOR_NTFY_TOKEN", "secret-token")
    from fastapi.testclient import TestClient
    body = TestClient(module.app).get("/api/v1/notifications/status").json()
    assert body["configured"] is True and "secret" not in str(body)

def test_notification_test_reports_channel_result(tmp_path, monkeypatch):
    import server.app as module
    monkeypatch.setattr(module, "DB_PATH", tmp_path / "x.db")
    monkeypatch.setenv("EDGEDOCTOR_NTFY_URL", "https://ntfy.sh")
    monkeypatch.setenv("EDGEDOCTOR_NTFY_TOPIC", "private-topic")
    monkeypatch.setattr(module, "publish_ntfy", lambda *args: "CHANNEL_ACCEPTED")
    from fastapi.testclient import TestClient
    result = TestClient(module.app).post("/api/v1/notifications/test")
    assert result.status_code == 200 and result.json()["status"] == "CHANNEL_ACCEPTED"
