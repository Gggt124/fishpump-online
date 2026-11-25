"""Runs the offline packet-encoding selftest of mqtt_mini.py under unittest (CI)."""
import unittest

import mqtt_mini


class MqttMiniTest(unittest.TestCase):
    def test_selftest(self):
        self.assertEqual(mqtt_mini.selftest(), 0)


if __name__ == "__main__":
    unittest.main()
