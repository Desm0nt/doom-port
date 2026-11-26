package com.xiaoboluo.d2r;

import java.util.Locale;

// [front] Adreno model number from the kernel's gpu_model, Android-free so javac and tests run it alone (VkDrivers.adrenoModel delegates here).
// Qualcomm's two-digit names ("Adreno22" on SG6275P / SG_CLIFFS7P, GL renderer "Adreno (TM) 22") are real models, not a missing number.
final class AdrenoModel {
    private AdrenoModel() { }

    // "Adreno750v2" -> 750; "Adreno22" -> 22; 0 = no readable model; -1 = a model that is not an Adreno.
    static int of(String gpu) {
        if (gpu == null || gpu.isEmpty()) return 0;
        String g = gpu.toLowerCase(Locale.ROOT); int at = g.indexOf("adreno"); if (at < 0) return -1;
        int i = at + 6, v = 0, n = 0;
        while (i < g.length() && !Character.isDigit(g.charAt(i)) && n == 0 && i < at + 12) i++;
        while (i < g.length() && Character.isDigit(g.charAt(i)) && n < 4) { v = v * 10 + (g.charAt(i++) - '0'); n++; }
        return n > 0 ? v : -1;
    }
    // a2xx .. a5xx: the legacy three-digit numbering Mesa Turnip does not support (a two-digit model is newer, not older).
    static boolean tooOldForTurnip(int model) { return model >= 100 && model < 600; }
}
