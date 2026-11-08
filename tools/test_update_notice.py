# Host unit test of UpdateNotice.java's pure helpers (BUGS_AND_FIXES 2.41): the version compare and the changelog section / light markdown of
# a release body (the bilingual v0.4.21 / v0.4.12 format: "## English" / "## Русский", a "---" and SHA-256 lines, a bilingual notice above).
# UpdateNotice.java is compiled as is against the SDK's android.jar (its Android classes are only loaded, never run) with a small test class.
# Usage: python tools/test_update_notice.py   (needs a JDK 17+: JAVA_HOME or javac / java on PATH; android.jar: ANDROID_JAR, or
# ANDROID_HOME / ANDROID_SDK_ROOT platforms/android-36)
import glob, os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JAVA_DIR = next(d for d in (os.path.join(ROOT, 'src', 'dm', 'android', 'app', 'src', 'main', 'java', 'com', 'xiaoboluo', 'd2r'),
                            os.path.join(ROOT, 'dm', 'android', 'app', 'src', 'main', 'java', 'com', 'xiaoboluo', 'd2r')) if os.path.isdir(d))
SRC = os.path.join(JAVA_DIR, 'UpdateNotice.java')

TEST = r'''
package com.xiaoboluo.d2r;
public class UpdateNoticeTest {
    static int fails;
    static void eq(Object got, Object want, String what) {
        if (!String.valueOf(got).equals(String.valueOf(want))) { fails++; System.out.println("FAIL " + what + ":\n  got  [" + got + "]\n  want [" + want + "]"); }
        else System.out.println("ok   " + what);
    }
    static String spans(UpdateNotice.Rich r) {
        StringBuilder sb = new StringBuilder();
        for (int[] s : r.spans) sb.append(s[2] == UpdateNotice.BOLD ? "B" : "C").append('[').append(r.text.substring(s[0], s[1])).append(']');
        return sb.toString();
    }
    public static void main(String[] a) throws Exception {
        // versions
        eq(UpdateNotice.compareVersions("v0.4.22", "0.4.21"), 1, "v0.4.22 > 0.4.21");
        eq(UpdateNotice.compareVersions("v0.4.21", "0.4.21"), 0, "v0.4.21 == 0.4.21");
        eq(UpdateNotice.compareVersions("v0.4.9", "0.4.10"), -1, "numeric, not string: 0.4.9 < 0.4.10");
        eq(UpdateNotice.compareVersions("v0.5", "0.4.99"), 1, "0.5 > 0.4.99 (missing component = 0)");
        eq(UpdateNotice.compareVersions("v0.4.21", "0.4.21-diag"), 0, "-diag does not count");
        eq(UpdateNotice.compareVersions("v0.4.18", "0.4.18-test2"), 1, "release > its -test2");
        eq(UpdateNotice.compareVersions("v0.4.18-test1", "0.4.17"), 1, "0.4.18-test1 > 0.4.17");
        eq(UpdateNotice.compareVersions("v1.0.0", "0.9.9"), 1, "1.0.0 > 0.9.9");
        eq(UpdateNotice.versionParts("nightly").length, 0, "no version in 'nightly'");
        eq(UpdateNotice.russianUi("uk") + " " + UpdateNotice.russianUi("ru") + " " + UpdateNotice.russianUi("de"), "true true false", "ru / uk -> Russian");
        // the changelog
        String body = String.join("\n", "## English", "", "**Settings now stay saved** (#31)", "- The game did not save `profile` settings.", "  - nested point",
            "", "", "**Install / update:** no setup re-run needed. See [INSTALL](https://github.com/x/y/blob/main/docs/INSTALL.md).", "",
            "## Русский", "", "**Настройки теперь сохраняются** (#31)", "- Игра не сохраняла настройки.", "", "---", "",
            "SHA-256 (DOOM-Mobile-v0.4.21-arm64.apk): 1db6daad", "SHA-256 (DOOM-Mobile-v0.4.21-arm64-android10.apk): cbca46de");
        String en = UpdateNotice.changelog(body.replace("\n", "\r\n"), "en"), ru = UpdateNotice.changelog(body, "ru");
        eq(en.contains("Русский") || en.contains("SHA-256") || en.contains("---"), false, "English section without Russian / --- / SHA");
        eq(ru.startsWith("**Настройки") && ru.endsWith("настройки.") && !ru.contains("English"), true, "Russian section for ru");
        eq(UpdateNotice.changelog(body, "uk").equals(ru), true, "uk -> Russian section");
        eq(UpdateNotice.changelog(body, "ja").equals(en), true, "other languages -> English section");
        UpdateNotice.Rich r = UpdateNotice.render(en);
        eq(r.text, "Settings now stay saved (#31)\n• The game did not save profile settings.\n    ◦ nested point\n\nInstall / update: no setup re-run needed. See INSTALL.",
            "markdown: bold, bullets, nested bullet, inline code, link text, one blank line");
        eq(spans(r), "B[Settings now stay saved]C[profile]B[Install / update:]", "spans: bold, code, bold");
        // no language sections: the whole body without the SHA tail
        eq(UpdateNotice.changelog("Fixes:\n* one\n\n---\nSHA-256 (x.apk): ab", "ru"), "Fixes:\n* one", "no sections: whole body, --- tail dropped");
        // a bilingual notice above the sections is kept; GitHub's [!IMPORTANT] marker and the quote marks are dropped
        String b12 = "> [!IMPORTANT]\n> **Re-run setup!**\n>\n> **Запустите setup!**\n\n## English\n\n### Fixed\n- crash\n\n## Русский\n\n- падение\n";
        UpdateNotice.Rich r12 = UpdateNotice.render(UpdateNotice.changelog(b12, "en"));
        eq(r12.text, "Re-run setup!\n\nЗапустите setup!\n\nFixed\n• crash", "preamble kept, [!IMPORTANT] dropped, ### heading bold");
        eq(spans(r12), "B[Re-run setup!]B[Запустите setup!]B[Fixed]", "preamble / heading spans");
        // only a Russian section: shown for English too
        eq(UpdateNotice.changelog("## Русский\n- только русский", "en"), "- только русский", "missing English section -> the other one");
        eq(UpdateNotice.render("a \\*not bold\\* `x` **unclosed").text, "a *not bold* x **unclosed", "escapes, unclosed bold kept as text");
        eq(UpdateNotice.changelog(null, "en") + "|" + UpdateNotice.render("").text, "|", "empty body");
        System.out.println(fails == 0 ? "ALL OK" : fails + " FAILED");
        System.exit(fails == 0 ? 0 : 1);
    }
}
'''

def tool(name):
    jh = os.environ.get('JAVA_HOME')
    if jh:
        p = os.path.join(jh, 'bin', name + ('.exe' if os.name == 'nt' else ''))
        if os.path.isfile(p): return p
    return shutil.which(name)

def android_jar():
    if os.environ.get('ANDROID_JAR'): return os.environ['ANDROID_JAR']
    for v in ('ANDROID_HOME', 'ANDROID_SDK_ROOT'):
        if os.environ.get(v):
            js = sorted(glob.glob(os.path.join(os.environ[v], 'platforms', 'android-*', 'android.jar')))
            if js: return js[-1]
    return None

def main():
    javac, java, aj = tool('javac'), tool('java'), android_jar()
    if not (javac and java and aj and os.path.isfile(aj)):
        print('SKIP: needs javac / java (JAVA_HOME) and android.jar (ANDROID_JAR or ANDROID_HOME)'); return 0
    with tempfile.TemporaryDirectory() as tmp:
        t = os.path.join(tmp, 'UpdateNoticeTest.java')
        open(t, 'w', encoding='utf-8').write(TEST)
        out = os.path.join(tmp, 'classes')
        r = subprocess.run([javac, '-encoding', 'UTF-8', '-source', '17', '-target', '17', '-nowarn', '-cp', aj, '-d', out, SRC, t])
        if r.returncode: print('javac failed'); return 1
        return subprocess.run([java, '-Dfile.encoding=UTF-8', '-Dstdout.encoding=UTF-8', '-cp', out + os.pathsep + aj, 'com.xiaoboluo.d2r.UpdateNoticeTest']).returncode

if __name__ == '__main__':
    sys.exit(main())
