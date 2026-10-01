#!/usr/bin/env python3
import configparser
import pathlib
import sys
from urllib.parse import urlsplit
import xml.etree.ElementTree as ET


desktop_path, icon_path, metainfo_path = map(pathlib.Path, sys.argv[1:4])
desktop = configparser.ConfigParser(interpolation=None)
desktop.read(desktop_path, encoding="utf-8")
entry = desktop["Desktop Entry"]
if entry.get("Icon") != "skyapo":
    raise SystemExit("desktop entry does not use the branded skyapo icon")
if entry.get("Exec") != "skyapo-ui":
    raise SystemExit("desktop entry does not launch skyapo-ui")
if not icon_path.is_file():
    raise SystemExit("branded icon asset is missing")

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
