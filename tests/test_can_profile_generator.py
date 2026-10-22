"""Configuration failures must stop the build, not silently widen CAN permissions."""
import copy
import importlib.util
from pathlib import Path
import unittest
import yaml
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('generator', ROOT/'tools/generate_can_profile.py')
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class ConfigurationTests(unittest.TestCase):
    def setUp(self):
        self.config = yaml.safe_load((ROOT/'config/can_devices.yaml').read_text())

    def test_duplicate_yaml_keys_are_errors(self):
        with self.assertRaises(ValueError):
            generator.load_config('command_timeout_us: 20000\ncommand_timeout_us: 1000000\n')

    def test_boot_protection_is_default(self):
        del self.config['protect_on_boot']
        self.assertTrue(generator.compile_profile(self.config)['boot'])

    def test_current_devices_and_protection(self):
        profile = generator.compile_profile(self.config)
        self.assertEqual(len(profile['filters']), 6)
        self.assertEqual(profile['stops'][0][4], [0]*8)
        self.assertEqual(profile['stops'][1][4], [255]*7+[253])
        self.assertEqual(profile['signature'], 0x9fb5fef5)

    def test_duplicate_channels(self):
        self.config['can_buses'][1]['channel'] = 3
        with self.assertRaises(ValueError): generator.compile_profile(self.config)

    def test_invalid_ids_and_types(self):
        for value in (0x800, -1, True, '0x200'):
            config = copy.deepcopy(self.config)
            config['can_buses'][0]['devices'][0]['command_id'] = value
            with self.assertRaises(ValueError): generator.compile_profile(config)

    def test_bad_protection_and_missing_policy(self):
        dev = self.config['can_buses'][0]['devices'][0]
        dev['protection_frame']['data'] = [256]
        with self.assertRaises(ValueError): generator.compile_profile(self.config)
        del dev['protection_frame']
        with self.assertRaises(ValueError): generator.compile_profile(self.config)

    def test_hardware_filter_limit(self):
        self.config['can_buses'][0]['devices'][0]['feedback_ids'] = list(range(17))
        with self.assertRaises(ValueError): generator.compile_profile(self.config)

    def test_shared_group_command_is_deduplicated(self):
        bus = self.config['can_buses'][0]
        extra = copy.deepcopy(bus['devices'][0]); extra['name'] = 'group_second_motor'
        extra['feedback_ids'] = [0x202]; bus['devices'].append(extra)
        profile = generator.compile_profile(self.config)
        self.assertEqual(len(profile['stops']), 6)
        self.assertEqual(len(profile['filters']), 7)
        extra['protection_frame']['data'][0] = 1
        with self.assertRaises(ValueError): generator.compile_profile(self.config)

    def test_extended_and_remote_filter(self):
        dev = self.config['can_buses'][0]['devices'][0]
        dev['frame_format'] = 'extended'; dev['command_id'] = 0x1234567
        dev['feedback_ids'] = [0x1234568]; dev['feedback_remote'] = True
        profile = generator.compile_profile(self.config)
        self.assertIn((3, 0x1234568, 3), profile['filters'])
        self.assertIn((3, 0x1234567, 1), profile['addresses'])

    def test_unknown_fields_do_not_silently_change_defaults(self):
        self.config['can_buses'][0]['devices'][0]['commmand_id'] = 1
        with self.assertRaises(ValueError): generator.compile_profile(self.config)

    def test_protection_capacity_is_not_usb_packet_capacity(self):
        bus = self.config['can_buses'][0]
        bus['devices'] = [dict(name=f'd{i}', command_id=i, feedback_ids=[],
            protection_frame={'data':[0]*8}) for i in range(32)]
        self.config['can_buses'] = [bus]
        self.assertEqual(len(generator.compile_profile(self.config)['stops']), 32)
        bus['devices'].append(dict(name='extra', command_id=32, feedback_ids=[],
            protection_frame={'data':[0]*8}))
        with self.assertRaises(ValueError): generator.compile_profile(self.config)


if __name__ == '__main__': unittest.main()
