package com.xiaoboluo.d2r;

import android.content.Context;
import java.util.Locale;

// [front] The unified front end's own texts (pause menu, gamepad dialog, saves) in Russian, English or Simplified Chinese (frontend-common, docs/FRONTEND_UNIFIED.md).
// Russian when the phone's language is Russian / Ukrainian / Belarusian, English otherwise; "front" prefs ui_lang ("" / ru / en / zh) overrides it.
// The games' own settings dialogs keep their string resources (DOOM: 13 languages).
final class Ui {
    static final String PREFS = "front";
    static boolean ru(Context c) {
        String l = c.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString("ui_lang", "");
        if (l.equals("ru")) return true;
        if (l.equals("en")) return false;
        String s = Locale.getDefault().getLanguage();
        return s.equals("ru") || s.equals("uk") || s.equals("be");
    }
    // Simplified Chinese (D3 issue #28): ui_lang "zh", or the phone's language zh with ui_lang "". The texts stay ru / en pairs in the code;
    // UiZh maps the English text to the Chinese one (missing = English).
    static boolean zh(Context c) {
        String l = c.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString("ui_lang", "");
        return l.isEmpty() ? Locale.getDefault().getLanguage().equals("zh") : l.equals("zh");
    }
    static String t(Context c, String ru, String en) {
        if (ru(c)) return ru;
        if (zh(c)) { String z = UiZh.get(en); if (z != null) return z; }
        return en;
    }
}
