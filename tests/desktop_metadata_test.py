#!/usr/bin/env python3
import configparser
import pathlib
import struct
import sys
from urllib.parse import urlsplit
import xml.etree.ElementTree as ET


desktop_path = pathlib.Path(sys.argv[1])
icon_argument = sys.argv[2]
metainfo_path = pathlib.Path(sys.argv[3])
desktop = configparser.ConfigParser(interpolation=None)
desktop.read(desktop_path, encoding="utf-8")
entry = desktop["Desktop Entry"]
if entry.get("Exec") != "skyapo-ui":
    raise SystemExit("desktop entry does not launch skyapo-ui")
if icon_argument:
    icon_path = pathlib.Path(icon_argument)
    if entry.get("Icon") != "skyapo":
        raise SystemExit("branded icon build lacks its desktop Icon entry")
    if not icon_path.is_file():
        raise SystemExit("branded icon asset is missing")
    png = icon_path.read_bytes()
    if png[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("application icon must be a PNG")
    width, height, _bit_depth, color_type = struct.unpack(">IIBB", png[16:26])
    if (width, height, color_type) != (256, 256, 6):
        raise SystemExit("application icon must be a 256x256 RGBA PNG")
elif "Icon" in entry:
    raise SystemExit(
        "desktop entry references an icon although branded artwork is disabled"
    )

root = ET.parse(metainfo_path).getroot()
if root.attrib.get("type") != "desktop-application":
    raise SystemExit("AppStream component is not a desktop application")
if root.findtext("id") != "org.skyapo.skyapo":
    raise SystemExit("AppStream component does not use its reverse-DNS ID")
launchable = root.find("launchable")
if launchable is None or launchable.text != desktop_path.name:
    raise SystemExit("AppStream launchable does not match desktop file")
for required in ("name", "summary", "metadata_license", "project_license"):
    if not root.findtext(required):
        raise SystemExit(f"AppStream metadata is missing {required}")


def validate_optional_homepage(component):
    homepage = component.find("url[@type='homepage']")
    if homepage is None:
        return
    address = (homepage.text or "").strip()
    parsed = urlsplit(address)
    if parsed.scheme != "https" or not parsed.netloc:
        raise ValueError(
            "AppStream homepage, when provided, must be an HTTPS URL"
        )
    if "equalizerapo" in address.casefold():
        raise ValueError(
            "AppStream homepage must not point to the upstream Equalizer APO project"
        )


validate_optional_homepage(root)

# Keep URL optional until SkyAPO has its own official homepage, and guard
# against reintroducing the unrelated upstream Equalizer APO project URL.
validate_optional_homepage(ET.fromstring("<component />"))
upstream_homepage = ET.fromstring(
    '<component><url type="homepage">'
    "https://sourceforge.net/projects/equalizerapo/"
    "</url></component>"
)
try:
    validate_optional_homepage(upstream_homepage)
except ValueError:
    pass
else:
    raise SystemExit(
        "AppStream metadata must reject the upstream Equalizer APO homepage"
    )
