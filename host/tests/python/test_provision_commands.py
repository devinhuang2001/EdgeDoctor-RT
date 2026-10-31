"""Execute the provisioning script's Python payload against a serial boundary."""
import contextlib
import io
import os
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import patch


class ProvisionCommandsTests(unittest.TestCase):
    def run_payload(self, password):
        script = Path(__file__).resolve().parents[2] / "scripts/provision-board-mqtt.ps1"
        code = script.read_text(encoding="utf-8").split("$py = @'", 1)[1].split("'@", 1)[0]
        commands = []

        class Serial:
            def __init__(self, *args, **kwargs):
                pass

            def write(self, data):
                commands.append(data.decode().strip())

            def read(self, size):
                return (commands[-1] + "\nedmqtt: saved\ncredentials saved\n").encode()

            def close(self):
                pass

        argv = ["provision", "--port", "test", "--ssid", "wifi", "--password", "wifi-pass",
                "--broker", "127.0.0.1", "--broker-port", "1883", "--site", "site", "--device", "pump"]
        output = io.StringIO()
        with patch.dict(sys.modules, {"serial": types.SimpleNamespace(Serial=Serial)}), \
             patch.object(sys, "argv", argv), patch("time.sleep"), \
             patch.dict(os.environ, {"EDGEDOCTOR_PROVISION_MQTT_USER": "operator",
                                     "EDGEDOCTOR_PROVISION_MQTT_PASSWORD": password}), \
             contextlib.redirect_stdout(output):
            exec(compile(code, str(script), "exec"), {"__name__": "__main__"})
        return commands, output.getvalue()

    def test_stop_configure_credentials_start_order_and_redaction(self):
        commands, output = self.run_payload("secret-password")
        stop = commands.index("edmqtt stop")
        self.assertEqual(commands[stop + 1:stop + 4], [
            "edmqtt config site pump 127.0.0.1 1883",
            'edmqtt auth "operator" "secret-password"', "edmqtt start"])
        self.assertNotIn("secret-password", output)

    def test_oversized_credentials_fail_before_serial_write(self):
        with self.assertRaisesRegex(SystemExit, "79-byte"):
            self.run_payload("p" * 100)


if __name__ == "__main__":
    unittest.main()
