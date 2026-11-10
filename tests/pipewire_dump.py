"""Parse PipeWire ``pw-dump`` snapshots and registry updates."""

import json


def pipewire_dump_objects(raw):
    """Fold concatenated pw-dump JSON arrays into the latest object snapshot.

    PipeWire can write its initial registry snapshot and registry updates as
    adjacent JSON arrays when globals change during the dump. A removed global
    is represented by an object whose ``info`` (or legacy ``props``) is null.
    """
    decoder = json.JSONDecoder()
    offset = 0
    objects = {}
    order = []
    document_count = 0

    while offset < len(raw):
        while offset < len(raw) and raw[offset].isspace():
            offset += 1
        if offset == len(raw):
            break

        document, offset = decoder.raw_decode(raw, offset)
        if not isinstance(document, list):
            raise ValueError("pw-dump JSON document is not an array")
        document_count += 1

        for item in document:
            if not isinstance(item, dict) or "id" not in item:
                raise ValueError("pw-dump array contains an invalid object")
            object_id = item["id"]
            if item.get("info", object()) is None or item.get("props", object()) is None:
                objects.pop(object_id, None)
                if object_id in order:
                    order.remove(object_id)
                continue
            if object_id not in objects:
                order.append(object_id)
            objects[object_id] = item

    if document_count == 0:
        raise ValueError("pw-dump produced no JSON documents")
    return [objects[object_id] for object_id in order if object_id in objects]
