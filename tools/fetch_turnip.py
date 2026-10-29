#!/usr/bin/env python3
"""Fetch the Turnip (Mesa) driver package the APK bundles as its default Vulkan driver.

  fetch_turnip.py [--out DIR]   (default DIR: work/driver-assets)

Downloads the pinned adrenotools package from The412Banner/Banners-Turnip (Mesa, MIT License), checks its SHA-256 and
unpacks it to DIR/drivers/turnip27/{libvulkan_freedreno.so, meta.json}. tools/build_apk.ps1 packages DIR as APK assets;
DmActivity copies the package to private storage on first start and uses it unless the gear dialog picks another driver
(the system driver stays the fallback when Turnip does not load or sees no GPU, e.g. on non-Adreno phones).
Nothing is downloaded when the package is already there.
"""
import argparse, hashlib, io, os, sys, urllib.request, zipfile

NAME = 'turnip27'
URL = 'https://github.com/The412Banner/Banners-Turnip/releases/download/v27.0.0-20261029/Turnip-v27.0.0-20261029.zip'
SHA256 = 'd413e41d53508cab28698548f77852f37846003727550b09814018e5b9a352b1'

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--out', default=os.path.join(os.path.dirname(__file__), '..', 'work', 'driver-assets'))
    out = os.path.join(ap.parse_args().out, 'drivers', NAME)
    lib = os.path.join(out, 'libvulkan_freedreno.so')
    if os.path.isfile(lib) and os.path.isfile(os.path.join(out, 'meta.json')):
        print(f'{NAME}: already in {out}'); return
    print(f'{NAME}: downloading {URL}')
    data = urllib.request.urlopen(URL, timeout=120).read()
    got = hashlib.sha256(data).hexdigest()
    if got != SHA256: sys.exit(f'{NAME}: SHA-256 mismatch ({got}), not using it')
    os.makedirs(out, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for n in ('libvulkan_freedreno.so', 'meta.json'):
            with open(os.path.join(out, n), 'wb') as f: f.write(z.read(n))
    print(f'{NAME}: unpacked to {out}')

if __name__ == '__main__':
    main()
