#!/usr/bin/env python3
"""Regression tests for PipeWire's multi-document pw-dump output."""

import unittest

from pipewire_dump import pipewire_dump_objects


class PipeWireDumpParserTest(unittest.TestCase):
    def test_single_snapshot(self):
        self.assertEqual(
            pipewire_dump_objects('[{"id":1,"type":"Node"}]'),
            [{"id": 1, "type": "Node"}],
        )

    def test_concatenated_snapshot_and_new_global(self):
        output = (
            '[{"id":1,"type":"Node"}]'
            '[{"id":2,"type":"Port"}]'
        )
        self.assertEqual(
            pipewire_dump_objects(output),
            [{"id": 1, "type": "Node"}, {"id": 2, "type": "Port"}],
        )

    def test_updates_replace_existing_global(self):
        output = (
            '[{"id":1,"type":"Node","info":{"props":{"name":"old"}}}]'
            '[{"id":1,"type":"Node","info":{"props":{"name":"new"}}}]'
        )
        self.assertEqual(
            pipewire_dump_objects(output),
            [{"id": 1, "type": "Node",
              "info": {"props": {"name": "new"}}}],
        )

    def test_removal_tombstone_removes_global(self):
        output = '[{"id":1,"type":"Node"}][{"id":1,"info":null}]'
        self.assertEqual(pipewire_dump_objects(output), [])

    def test_invalid_document_is_reported(self):
        with self.assertRaises(ValueError):
            pipewire_dump_objects('[{"id":1}] trailing')


if __name__ == "__main__":
    unittest.main()
