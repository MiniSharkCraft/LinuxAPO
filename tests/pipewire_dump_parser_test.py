#!/usr/bin/env python3
"""Regression tests for PipeWire's multi-document pw-dump output."""

import json
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from pipewire_e2e_test import destroy_playback_internal_link

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


class PlaybackFaultTopologyTest(unittest.TestCase):
    def exercise(self, edges):
        nodes = [
            {"id": node_id, "type": "PipeWire:Interface:Node",
             "info": {"props": {"node.name": name}}}
            for node_id, name in enumerate((
                "skyapo.playback", "skyapo.playback.dsp",
                "skyapo.playback.stream"), start=1)
        ]
        links = [
            {"id": link_id, "type": "PipeWire:Interface:Link",
             "info": {"props": {"link.output.node": output,
                                "link.input.node": input_node}}}
            for link_id, (output, input_node) in enumerate(edges, start=10)
        ]
        mocked = patch("pipewire_e2e_test.run", return_value=SimpleNamespace(
            stdout=json.dumps(nodes + links)))
        return mocked

    def test_destroys_dsp_link_only_after_verifying_adapter_path(self):
        with self.exercise([(1, 2), (2, 3)]) as run:
            self.assertEqual(destroy_playback_internal_link(
                "pw-cli", "pw-dump", {}), "11")
            self.assertEqual(run.call_count, 2)
            self.assertEqual(run.call_args.args[0],
                             ["pw-cli", "destroy", "11"])

    def test_bypass_or_incomplete_graph_is_never_destroyed(self):
        for edges in ([(1, 3)], [(2, 3)], [(1, 2)]):
            with self.subTest(edges=edges), self.exercise(edges) as run:
                with self.assertRaises(RuntimeError):
                    destroy_playback_internal_link("pw-cli", "pw-dump", {})
                self.assertEqual(run.call_count, 1)


if __name__ == "__main__":
    unittest.main()
