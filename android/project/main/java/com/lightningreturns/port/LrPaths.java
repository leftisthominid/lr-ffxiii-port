package com.lightningreturns.port;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Environment;

import java.io.File;

/** Folder settings shared by the launcher and the game activity. */
final class LrPaths {
    static final String PREFS = "lr_paths";
    static final String KEY_GAME = "game_root";
    static final String KEY_USER = "user_root";

    private LrPaths() {}

    static String defaultGameRoot() {
        return new File(Environment.getExternalStorageDirectory(), "LightningReturns").getAbsolutePath();
    }

    /** Saves, settings (resolution.txt, ...), lightningreturns.toml and logs. Never inside the game folder. */
    static String defaultUserRoot(String gameRoot) {
        File g = new File(gameRoot);
        File parent = g.getParentFile() != null ? g.getParentFile() : Environment.getExternalStorageDirectory();
        return new File(parent, g.getName() + "-userdata").getAbsolutePath();
    }

    static String gameRoot(Context c) {
        return prefs(c).getString(KEY_GAME, defaultGameRoot());
    }

    static String userRoot(Context c) {
        String s = prefs(c).getString(KEY_USER, null);
        return s != null && !s.isEmpty() ? s : defaultUserRoot(gameRoot(c));
    }

    static void save(Context c, String gameRoot, String userRoot) {
        prefs(c).edit().putString(KEY_GAME, gameRoot).putString(KEY_USER, userRoot).apply();
    }

    static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    /** True when the folder holds the 360 game's default.xex (any case). */
    static boolean looksLikeGameRoot(String root) {
        File[] files = new File(root).listFiles();
        if (files == null) return false;
        for (File f : files) {
            if (f.isFile() && f.getName().equalsIgnoreCase("default.xex")) return true;
        }
        return false;
    }
}
