package com.xiaoboluo.d2r;

import android.content.Context;
import java.util.Locale;

// [front] The unified front end's own texts (pause menu, gamepad dialog, saves) in Russian or English (frontend-common, docs/FRONTEND_UNIFIED.md).
// Russian when the phone's language is Russian / Ukrainian / Belarusian, English otherwise; "front" prefs ui_lang ("" / ru / en) overrides it.
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
    static String t(Context c, String ru, String en) { return ru(c) ? ru : en; }
}
