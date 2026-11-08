#!/usr/bin/env python3
"""Fetch the Vulkan driver packages the APK bundles (Turnip default + experimental options for other GPUs).

  fetch_turnip.py [--out DIR] [--zips ZIPDIR]   (default DIR: work/driver-assets, ZIPDIR: work/drivers)

Every package is a pinned adrenotools zip (meta.json + the driver .so, bionic / Android builds only), checked against its SHA-256
and unpacked to DIR/drivers/<name>/ (the .so, meta.json and the package's text files such as NOTICE.txt). The zip is kept in ZIPDIR
(reused offline when its SHA-256 matches). tools/build_apk.ps1 packages DIR as APK assets; DmActivity.extractBundledDrivers copies
to private storage only the packages that fit the phone (GPU family, glibc, meta.json minApi), so an Adreno phone gets the Turnip
builds and not the Mali / Xclipse ones (BUGS_AND_FIXES 2.39). turnip27 stays the default on Adreno 6xx+ (DmActivity.defaultDriver);
RADV-Xclipse and the PanVK builds are only offered in the gear dialog (the system driver is the default there).
A package is not fetched again when DIR/.stamps/<name> already holds its SHA-256.
"""
import argparse, hashlib, io, os, re, shutil, sys, urllib.request, zipfile

GH = 'https://github.com/'
PACKAGES = [   # (folder name, url, sha256 of the zip); licences: docs/THIRD_PARTY.md
    ('turnip27', GH + 'The412Banner/Banners-Turnip/releases/download/v27.0.0-20261029/Turnip-v27.0.0-20261029.zip',
     'd413e41d53508cab28698548f77852f37846003727550b09814018e5b9a352b1'),
    ('turnip27-a710', GH + 'The412Banner/Banners-Turnip/releases/download/v27.0.0-20261029/Turnip-v27.0.0-20261029-710-720-Test.zip',
     '4927023e818b81fe19e451549cf83290dc3545f28b537a9bf1f348b060747615'),
    ('turnip27-a8xx', GH + 'The412Banner/Banners-Turnip/releases/download/v27.0.0-20261029/Turnip-v27.0.0-20261029-A8xx.zip',
     'b1126d1a320624101757c7728179b80590d19d17d38a2eb5d85eb5dfd50df82a'),
    ('turnip27-8g2', GH + 'The412Banner/Banners-Turnip/releases/download/v27.0.0-20261029/Turnip-v27.0.0-20261029-8G2-OneUI.zip',
     'e7a9c8c12ccddd39b0316fae747350964096a6622630f1191bbab02208f356b3'),
    # Samsung Xclipse (Exynos 2200 / 2400 / 2500), Mesa RADV, MIT; meta.json minApi 34 (Android 14+)
    ('radv-xclipse-1.8.0', GH + 'JimVulkan/radv-xclipse/releases/download/v1.8.0/radv-xclipse-27.0.0-devel-269238e.zip',
     '0545b1b24817cc0ed4a9e4fc440a7080cbaf69a9ce5d1e4bb22b0e697129c223'),
    # Mali Bifrost / Valhall on the stock kbase kernel driver, Mesa PanVK, MIT; minApi 31
    ('panvk-jim-1.6.0', GH + 'JimVulkan/mali-panvk/releases/download/v1.6.0/mali-panvk-27.0.0-devel-r10.zip',
     '7e97db4d79d81ec9461d1e0676b62a865561cb56e21c151693aa25ffbe749f1c'),
    # the same family, another PanVK-kbase build (the Android -adrenotools zip, not the glibc -EMULATOR one); minApi 29
    ('panvk-zenith-26.3.1', GH + 'zenithblue-oss/panvk-kbase-android/releases/download/panvk-kbase-v26.3.1/Mesa-PanVK-Android-v26.3.1-795ca7ff-adrenotools.zip',
     '7e2e9987bbebefb06f52b2570e61e6208503d4d3a10a395b97aea9b6e1fad9e4'),
]
KEEP = re.compile(r'.+(\.so|\.so\.[0-9][0-9.]*|\.json|\.txt)$')   # what DmActivity.unzipDriver keeps too
GLIBC = ('libc.so.6', 'libm.so.6', 'libpthread.so.0', 'libdl.so.2')

def get_zip(url, sha, zips):
    path = os.path.join(zips, url.rsplit('/', 1)[1])
    if os.path.isfile(path):
        data = open(path, 'rb').read()
        if hashlib.sha256(data).hexdigest() == sha: return data
    print(f'  downloading {url}')
    data = urllib.request.urlopen(url, timeout=300).read()
    got = hashlib.sha256(data).hexdigest()
    if got != sha: raise RuntimeError(f'SHA-256 mismatch ({got})')
    os.makedirs(zips, exist_ok=True)
    with open(path, 'wb') as f: f.write(data)
    return data

def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'work')
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(root, 'driver-assets'))
    ap.add_argument('--zips', default=os.path.join(root, 'drivers'))
    a = ap.parse_args()
    stamps = os.path.join(a.out, '.stamps'); os.makedirs(stamps, exist_ok=True)
    failed = 0
    for name, url, sha in PACKAGES:
        out = os.path.join(a.out, 'drivers', name); stamp = os.path.join(stamps, name)
        if os.path.isfile(stamp) and open(stamp).read().strip() == sha and os.path.isfile(os.path.join(out, 'meta.json')):
            print(f'{name}: already in {out}'); continue
        try:
            data = get_zip(url, sha, a.zips)
            if os.path.isdir(out): shutil.rmtree(out)
            os.makedirs(out)
            with zipfile.ZipFile(io.BytesIO(data)) as z:
                for n in z.namelist():
                    b = n.rsplit('/', 1)[-1]
                    if n.endswith('/') or not KEEP.match(b): continue
                    with open(os.path.join(out, b), 'wb') as f: f.write(z.read(n))
            if b'"abi"' in open(os.path.join(out, 'meta.json'), 'rb').read() and b'glibc' in open(os.path.join(out, 'meta.json'), 'rb').read():
                raise RuntimeError('meta.json says it is a glibc build')
            for b in os.listdir(out):   # an Android (bionic) library: no glibc sonames among its strings
                if b.endswith('.so') and any(g.encode() + b'\0' in open(os.path.join(out, b), 'rb').read() for g in GLIBC):
                    raise RuntimeError(f'{b} needs glibc')
            with open(stamp, 'w') as f: f.write(sha + '\n')
            print(f'{name}: unpacked to {out}')
        except Exception as e:
            failed += 1; print(f'{name}: NOT bundled: {e}')
            if os.path.isdir(out): shutil.rmtree(out)
    sys.exit(1 if failed else 0)

if __name__ == '__main__':
    main()
