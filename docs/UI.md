# User interface status

There is no GUI yet. The original Equalizer APO `Editor` source is present in the pinned upstream checkout, but it has not yet had a complete toolkit, portability, dependency-license or daemon-integration audit. The roadmap requires that audit before choosing the porting boundary. Future UI code must communicate with `skyapod`; it must never host the realtime audio graph itself, and closing the editor must not stop audio.
