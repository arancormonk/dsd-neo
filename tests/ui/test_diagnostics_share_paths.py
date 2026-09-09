# SPDX-License-Identifier: GPL-3.0-or-later
"""Source contract for the diagnostics provider; device URI grants need instrumentation."""
from pathlib import Path
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[2]
package = root / 'android/package'
paths = ET.parse(package / 'res/xml/diagnostics_paths.xml').getroot()
assert len(paths) == 1
assert paths[0].tag == 'cache-path'
assert paths[0].attrib == {'name': 'diagnostics', 'path': 'diagnostics/'}
android = '{http://schemas.android.com/apk/res/android}'
providers = ET.parse(package / 'AndroidManifest.xml').findall('.//provider')
provider = next(p for p in providers if p.get(android + 'authorities') == '${applicationId}.diagnostics')
assert provider.get(android + 'exported') == 'false'
assert provider.get(android + 'grantUriPermissions') == 'true'
assert provider.find('meta-data').get(android + 'resource') == '@xml/diagnostics_paths'
# Resolving known private files against the only exposed root must fail.
cache = Path('/app/cache')
exposed = cache / paths[0].get('path')
for private in (Path('/app/files/systems.json'), Path('/app/files/prefs.ini'),
                Path('/app/files/imports/channels.csv'), cache / 'imports/channels.csv'):
    assert not private.is_relative_to(exposed)
source = (package / 'src/io/github/arancormonk/dsdneo/DiagnosticsShare.kt').read_text()
assert 'fun share(context: Context, text: String, title: String)' in source
assert 'File.createTempFile(' in source
assert 'Intent.ACTION_SEND' in source
assert 'Intent.FLAG_GRANT_READ_URI_PERMISSION' in source
assert 'FLAG_GRANT_WRITE_URI_PERMISSION' not in source
assert '60 * 60 * 1000' in source
print('diagnostics provider source contract: passed')
