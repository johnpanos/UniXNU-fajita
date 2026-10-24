#!/usr/bin/env python3
# mkmeta.py OUT UUID BUNDLEID : write the containermanagerd bundle-container metadata plist for /private/var/containers/Bundle/Application/UUID.
# Without it containermanagerd deletes the (unknown) bundle container at boot, and registered apps silently fail to launch.
import plistlib, sys
out, u, i = sys.argv[1:4]
plistlib.dump({'MCMMetadataIdentifier': i, 'MCMMetadataInfo': {'com.apple.MobileInstallation.ContentProtectionClass': -1}, 'MCMMetadataVersion': 6,
    'MCMMetadataUserIdentity': {'posixUID': 33, 'posixGID': 33, 'personaUniqueString': 'CB75D6E8-DBB0-40EA-8EB0-04988FCEAE8B', 'type': 0, 'version': '2'},
    'MCMMetadataContentClass': 1, 'MCMMetadataUUID': u, 'MCMMetadataSchemaVersion': 1, 'MCMMetadataActiveDPClass': -1}, open(out, 'wb'), fmt=plistlib.FMT_BINARY)
