package com.lightningreturns.port;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.util.Log;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

/**
 * The touch layer over the game (the black margins left and right of the 16:9 picture):
 * <ul>
 *   <li>the Ark button (clock ring, top left) opens the Ark menu: saves export / import, automatic
 *       save backup, touch controls layout, performance readout, screenshot, quit;</li>
 *   <li>the camera button (top right) takes a screenshot;</li>
 *   <li>the on-screen controls, in the margins or over the picture (merged into controller 1 by
 *       lr_hooks.cpp, so a connected controller keeps working alongside);</li>
 *   <li>the performance readout (frame rate and frame time) in the left margin.</li>
 * </ul>
 * The on-screen buttons show the prompts the game uses (Xbox 360 or PS3, from the game's own
 * Settings), nothing here chooses them. The Ark menu also works with a controller: up / down,
 * A to select, B to close; the game sees no buttons while it is open.
 */
final class LrTouch {
    private static final String TAG = "LrTouch";
    static final int REQUEST_EXPORT = 7101;
    static final int REQUEST_IMPORT = 7102;

    // XInput button bits.
    static final int UP = 0x0001, DOWN = 0x0002, LEFT = 0x0004, RIGHT = 0x0008, START = 0x0010,
            BACK = 0x0020, L3 = 0x0040, R3 = 0x0080, LB = 0x0100, RB = 0x0200, A = 0x1000,
            B = 0x2000, X = 0x4000, Y = 0x8000;
    // Pseudo bits for the triggers (sent as trigger values, not buttons).
    static final int LT = 0x10000, RT = 0x20000;

    static final int CONTROLS_ON = 0, CONTROLS_OFF = 1;
    static final float[] SIZE_SCALE = {0.85f, 1f, 1.15f};
    static final int READOUT_OFF = 0, READOUT_FPS = 1, READOUT_FULL = 2;
    static final int BACKUP_DOWNLOADS = 0, BACKUP_CHOSEN = 1, BACKUP_OFF = 2;

    // Palette: graphite glass, a gold hairline, ivory text.
    static final int GLASS = 0xD0101216, GLASS_PRESSED = 0xE03A3220, GOLD = 0xFFC2A66B,
            GOLD_DIM = 0x80C2A66B, IVORY = 0xFFEDE6D6, IVORY_DIM = 0x99EDE6D6, SHADE = 0xB0000000;

    static native void nativeSetPad(boolean active, int buttons, int lt, int rt, int lx, int ly, int rx, int ry);
    static native void nativeMenuOpen(boolean open);
    static native int nativeMenuButtons();
    static native boolean nativePhysicalActivity();
    static native float[] nativeStats();
    static native float[] nativeFrameTimes();
    static native boolean nativePs3Prompts();
    static native long nativeGameFrames();
    static native void nativeScreenshot();

    private static LrTouch sInstance;

    final LrActivity activity;
    final String userRoot;
    final SharedPreferences prefs;
    final Handler handler = new Handler(Looper.getMainLooper());
    final float dp;
    final OverlayView overlay;
    final ArkMenu menu;
    boolean nativeReady = true;
    int controls;
    int touchSize;
    boolean hiddenByPad;  // a controller is in use: the touch controls are out of the way
    int readout;
    int backup;
    boolean ps3;
    float[] stats = {0, 0, 0};
    float[] frameTimes = new float[0];
    long savesStamp = -1, pendingStamp = -1;
    boolean resumed;
    boolean gameStarted;  // the game's first frame is up (until then: the loading screen)
    long loadingStart = System.currentTimeMillis();

    static void attach(LrActivity activity, String userRoot) {
        try {
            sInstance = new LrTouch(activity, userRoot);
        } catch (RuntimeException e) {
            Log.e(TAG, "touch layer failed", e);
        }
    }

    static LrTouch get() {
        return sInstance;
    }

    private LrTouch(LrActivity activity, String userRoot) {
        this.activity = activity;
        this.userRoot = userRoot;
        prefs = activity.getSharedPreferences("lr_touch", Context.MODE_PRIVATE);
        dp = activity.getResources().getDisplayMetrics().density;
        controls = prefs.getInt("controls2", CONTROLS_ON);
        touchSize = prefs.getInt("touch_size", 1);
        readout = prefs.getInt("readout", READOUT_FULL);
        backup = prefs.getInt("backup", BACKUP_DOWNLOADS);
        overlay = new OverlayView(activity);
        menu = new ArkMenu(activity);
        menu.setVisibility(View.GONE);
        ViewGroup layout = (ViewGroup) org.libsdl.app.SDLActivity.getContentView();
        layout.addView(overlay, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        layout.addView(menu, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        savesStamp = newestSave(savesRoot());
    }

    void onResume() {
        resumed = true;
        handler.removeCallbacks(tick);
        handler.post(tick);
    }

    void onPause() {
        resumed = false;
        handler.removeCallbacks(tick);
        sendPad(false, 0, 0, 0, 0, 0);
    }

    /** 10 times a second: readout, prompts style, controller presses for the menu, backups. */
    private int ticks;
    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            if (!resumed) return;
            ++ticks;
            try {
                if (nativeReady) {
                    if (!gameStarted) {
                        if (nativeGameFrames() > 0) gameStarted = true;
                        overlay.invalidate();  // the loading screen's ring turns
                    }
                    float[] s = nativeStats();
                    if (s != null && s.length == 3) stats = s;
                    if (readout == READOUT_FULL) {
                        float[] f = nativeFrameTimes();
                        if (f != null) frameTimes = f;
                    }
                    boolean p = nativePs3Prompts();
                    if (p != ps3) {
                        ps3 = p;
                        overlay.invalidate();
                    }
                    if (menu.getVisibility() == View.VISIBLE) {
                        int pressed = nativeMenuButtons();
                        if (pressed != 0) menu.onPad(pressed);
                    } else if (nativePhysicalActivity() && controls == CONTROLS_ON && !hiddenByPad) {
                        // A controller is in use: the touch controls get out of the way until the
                        // screen is touched again (as Xbox cloud gaming does).
                        hiddenByPad = true;
                        overlay.resetPointers();
                        overlay.invalidate();
                    }
                }
            } catch (UnsatisfiedLinkError e) {
                nativeReady = false;
                Log.e(TAG, "game library not loaded", e);
            }
            if (readout != READOUT_OFF) overlay.invalidate();
            if (ticks % 40 == 0) checkAutoBackup();
            handler.postDelayed(this, 100);
        }
    };

    void sendPad(boolean active, int buttons, int lx, int ly, int rx, int ry) {
        if (!nativeReady) return;
        try {
            nativeSetPad(active, buttons & 0xFFFF, (buttons & LT) != 0 ? 255 : 0,
                    (buttons & RT) != 0 ? 255 : 0, lx, ly, rx, ry);
        } catch (UnsatisfiedLinkError e) {
            nativeReady = false;
        }
    }

    void setTouchSize(int size) {
        touchSize = size;
        prefs.edit().putInt("touch_size", size).apply();
        overlay.builtFor = -1;
        overlay.invalidate();
    }

    void setControls(int mode) {
        controls = mode;
        hiddenByPad = false;
        prefs.edit().putInt("controls2", mode).apply();
        overlay.resetPointers();
        overlay.invalidate();
    }

    void setReadout(int mode) {
        readout = mode;
        prefs.edit().putInt("readout", mode).apply();
        overlay.invalidate();
    }

    void openMenu() {
        overlay.resetPointers();
        menu.open();
        try {
            nativeMenuOpen(true);
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    void closeMenu() {
        menu.setVisibility(View.GONE);
        try {
            nativeMenuOpen(false);
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    void screenshot() {
        try {
            nativeScreenshot();
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    static String controlsName(int mode) {
        return mode == CONTROLS_OFF ? "Off" : "On (hide while a controller is used)";
    }

    static String sizeName(int size) {
        return size == 0 ? "Small" : size == 2 ? "Large" : "Medium";
    }

    static String readoutName(int mode) {
        switch (mode) {
            case READOUT_FPS: return "Frame rate";
            case READOUT_FULL: return "Frame rate + frame time";
            default: return "Off";
        }
    }

    String backupName(int mode) {
        switch (mode) {
            case BACKUP_CHOSEN: return prefs.getString("backup_uri", null) != null ? "Chosen file" : "Chosen file (pick one with Export saves to...)";
            case BACKUP_OFF: return "Off";
            default: return "Downloads";
        }
    }

    // ---------------------------------------------------------------------------------------
    // Saves: <user data>/B13EBABEBABEBABE/5351081B, the same layout as the PC port's
    // Documents\lightningreturns, so a zip goes either way.
    // ---------------------------------------------------------------------------------------
    static final String SAVE_PREFIX = "B13EBABEBABEBABE/5351081B/";

    File savesRoot() {
        return new File(userRoot, SAVE_PREFIX);
    }

    static File downloadsZip() {
        return new File(new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
                "LightningReturns"), "LightningReturns-saves.zip");
    }

    static long newestSave(File dir) {
        long newest = 0;
        File[] files = dir.listFiles();
        if (files == null) return 0;
        for (File f : files) {
            newest = Math.max(newest, f.isDirectory() ? newestSave(f) : f.lastModified());
        }
        return newest;
    }

    static void collect(File dir, String rel, List<String> out) {
        File[] files = dir.listFiles();
        if (files == null) return;
        for (File f : files) {
            String name = rel + f.getName();
            if (f.isDirectory()) collect(f, name + "/", out);
            else out.add(name);
        }
    }

    /** Writes the saves zip; returns the number of save files. */
    int writeSavesZip(OutputStream os) throws IOException {
        List<String> files = new ArrayList<>();
        collect(savesRoot(), "", files);
        int saves = 0;
        try (ZipOutputStream zip = new ZipOutputStream(os)) {
            zip.putNextEntry(new ZipEntry("README.txt"));
            zip.write(("Lightning Returns saves (the port's own format, phone and PC alike).\r\n"
                    + "PC: extract this zip into Documents\\lightningreturns (merge the folders).\r\n"
                    + "Phone: Ark > Import saves, then pick this zip.\r\n"
                    + "Made " + new SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.US).format(new Date()) + ".\r\n")
                    .getBytes("UTF-8"));
            zip.closeEntry();
            byte[] buf = new byte[64 * 1024];
            for (String rel : files) {
                zip.putNextEntry(new ZipEntry(SAVE_PREFIX + rel));
                try (InputStream in = new FileInputStream(new File(savesRoot(), rel))) {
                    for (int n; (n = in.read(buf)) > 0; ) zip.write(buf, 0, n);
                }
                zip.closeEntry();
                if (rel.contains("ff13-")) ++saves;
            }
        }
        return saves;
    }

    /** Export to Download/LightningReturns/LightningReturns-saves.zip (replaces the last one). */
    String exportToDownloads() {
        File target = downloadsZip();
        File tmp = new File(target.getParentFile(), target.getName() + ".tmp");
        try {
            target.getParentFile().mkdirs();
            int n;
            try (OutputStream os = new FileOutputStream(tmp)) {
                n = writeSavesZip(os);
            }
            if (target.exists() && !target.delete()) throw new IOException("cannot replace " + target);
            if (!tmp.renameTo(target)) throw new IOException("cannot write " + target);
            return "Saved " + n + " save file(s) to Download/LightningReturns/" + target.getName();
        } catch (IOException e) {
            tmp.delete();
            Log.e(TAG, "export failed", e);
            return "Export failed: " + e.getMessage();
        }
    }

    String exportToUri(Uri uri) {
        try (OutputStream os = activity.getContentResolver().openOutputStream(uri, "wt")) {
            if (os == null) throw new IOException("cannot open the file");
            int n = writeSavesZip(os);
            return "Saved " + n + " save file(s) to the chosen file";
        } catch (IOException | SecurityException e) {
            Log.e(TAG, "export failed", e);
            return "Export failed: " + e.getMessage();
        }
    }

    void pickExport() {
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("application/zip");
        i.putExtra(Intent.EXTRA_TITLE, "LightningReturns-saves.zip");
        try {
            activity.startActivityForResult(i, REQUEST_EXPORT);
        } catch (Exception e) {
            menu.status("No file picker on this phone");
        }
    }

    void pickImport() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip", "application/x-zip-compressed", "application/octet-stream"});
        try {
            activity.startActivityForResult(i, REQUEST_IMPORT);
        } catch (Exception e) {
            menu.status("No file picker on this phone");
        }
    }

    boolean onActivityResult(int request, int result, Intent data) {
        if (request != REQUEST_EXPORT && request != REQUEST_IMPORT) return false;
        Uri uri = data != null ? data.getData() : null;
        if (result != Activity.RESULT_OK || uri == null) {
            menu.status("Cancelled");
            return true;
        }
        if (request == REQUEST_EXPORT) {
            try {
                activity.getContentResolver().takePersistableUriPermission(uri,
                        Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            } catch (SecurityException ignored) {
            }
            prefs.edit().putString("backup_uri", uri.toString()).apply();
            runInBackground(() -> exportToUri(uri) + "; automatic backups can go there now");
        } else {
            runInBackground(() -> importFrom(uri));
        }
        return true;
    }

    interface Job {
        String run();
    }

    void runInBackground(Job job) {
        menu.status("Working...");
        new Thread(() -> {
            String msg = job.run();
            handler.post(() -> menu.status(msg));
        }, "LR saves").start();
    }

    /** Checks every file first, backs up the current saves, installs, restarts the game. */
    String importFrom(Uri uri) {
        File tmp = new File(activity.getCacheDir(), "import.zip");
        try {
            try (InputStream in = activity.getContentResolver().openInputStream(uri);
                 OutputStream out = new FileOutputStream(tmp)) {
                if (in == null) throw new IOException("cannot open the file");
                byte[] buf = new byte[64 * 1024];
                for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
            }
            List<String[]> entries = new ArrayList<>();  // {zip name, path below 5351081B/}
            int saves = 0;
            try (ZipFile zip = new ZipFile(tmp)) {
                for (java.util.Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
                    ZipEntry z = e.nextElement();
                    String name = z.getName().replace('\\', '/');
                    if (z.isDirectory() || name.equalsIgnoreCase("README.txt")) continue;
                    int at = name.toUpperCase(Locale.US).indexOf("5351081B/");
                    String rel = at >= 0 ? name.substring(at + 9) : null;
                    if (rel == null || rel.isEmpty() || rel.contains("..") || rel.startsWith("/")) {
                        return "Not a Lightning Returns saves zip (" + name + "); nothing changed";
                    }
                    // Every file must be readable.
                    try (InputStream in = zip.getInputStream(z)) {
                        byte[] buf = new byte[64 * 1024];
                        while (in.read(buf) > 0) { }
                    }
                    if (rel.contains("ff13-")) ++saves;
                    entries.add(new String[] {z.getName(), rel});
                }
                if (saves == 0) return "No saves in that zip; nothing changed";
                // Back up what is on the phone now.
                File backups = new File(userRoot, "save_backups");
                backups.mkdirs();
                File bak = new File(backups, "saves-" + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date()) + ".zip");
                try (OutputStream os = new FileOutputStream(bak)) {
                    writeSavesZip(os);
                }
                deleteTree(savesRoot());
                byte[] buf = new byte[64 * 1024];
                for (String[] e : entries) {
                    File out = new File(savesRoot(), e[1]);
                    out.getParentFile().mkdirs();
                    try (InputStream in = zip.getInputStream(zip.getEntry(e[0]));
                         OutputStream os = new FileOutputStream(out)) {
                        for (int n; (n = in.read(buf)) > 0; ) os.write(buf, 0, n);
                    }
                }
            }
            savesStamp = newestSave(savesRoot());
            handler.postDelayed(() -> {
                activity.lrRestartApp(0);
                Process.killProcess(Process.myPid());
            }, 1500);
            return "Imported " + saves + " save file(s) (the old ones are in save_backups); restarting...";
        } catch (IOException | SecurityException e) {
            Log.e(TAG, "import failed", e);
            return "Import failed, nothing changed: " + e.getMessage();
        } finally {
            tmp.delete();
        }
    }

    static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) for (File c : children) deleteTree(c);
        f.delete();
    }

    /** After the game saved (the newest save file changed and then stayed for a while). */
    void checkAutoBackup() {
        if (backup == BACKUP_OFF) return;
        long now = newestSave(savesRoot());
        if (now == savesStamp) {
            pendingStamp = -1;
            return;
        }
        if (now != pendingStamp) {
            pendingStamp = now;  // wait one more round: the game may still be writing
            return;
        }
        savesStamp = now;
        pendingStamp = -1;
        final String uri = prefs.getString("backup_uri", null);
        final int mode = backup;
        new Thread(() -> {
            String msg = mode == BACKUP_CHOSEN && uri != null ? exportToUri(Uri.parse(uri)) : exportToDownloads();
            Log.i(TAG, "automatic backup: " + msg);
        }, "LR backup").start();
    }

    // ---------------------------------------------------------------------------------------
    // Drawing helpers.
    // ---------------------------------------------------------------------------------------
    static Paint paint(int color, Paint.Style style, float stroke) {
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setColor(color);
        p.setStyle(style);
        p.setStrokeWidth(stroke);
        return p;
    }

    /** The Ark mark: a clock ring with twelve ticks and two hands. */
    static void drawRing(Canvas c, float cx, float cy, float r, Paint line) {
        c.drawCircle(cx, cy, r, line);
        for (int i = 0; i < 12; ++i) {
            double a = Math.PI * 2 * i / 12;
            float in = i % 3 == 0 ? r * 0.72f : r * 0.84f;
            c.drawLine(cx + (float) Math.sin(a) * in, cy - (float) Math.cos(a) * in,
                    cx + (float) Math.sin(a) * r * 0.95f, cy - (float) Math.cos(a) * r * 0.95f, line);
        }
        c.drawLine(cx, cy, cx, cy - r * 0.55f, line);
        c.drawLine(cx, cy, cx + r * 0.38f, cy + r * 0.12f, line);
    }

    // ---------------------------------------------------------------------------------------
    // The overlay: Ark and camera buttons, readout, on-screen controls.
    // ---------------------------------------------------------------------------------------
    final class OverlayView extends View {
        final Paint glass = paint(GLASS, Paint.Style.FILL, 0);
        final Paint pressed = paint(GLASS_PRESSED, Paint.Style.FILL, 0);
        final Paint gold = paint(GOLD, Paint.Style.STROKE, 1.2f * dp);
        final Paint goldDim = paint(GOLD_DIM, Paint.Style.STROKE, 1f * dp);
        final Paint text = paint(IVORY, Paint.Style.FILL, 0);
        final Paint textDim = paint(IVORY_DIM, Paint.Style.FILL, 0);
        final Paint knob = paint(0x60EDE6D6, Paint.Style.FILL, 0);

        // Controls (rebuilt on size or mode changes).
        final List<Ctl> ctls = new ArrayList<>();
        final RectF ark = new RectF(), camera = new RectF(), graph = new RectF();
        float readoutX, readoutY;
        int builtFor = -1, builtW, builtH;
        // Pointer tracking: pointer id -> control index (sticks and the strip buttons capture).
        final android.util.SparseIntArray captured = new android.util.SparseIntArray();
        final android.util.SparseArray<float[]> points = new android.util.SparseArray<>();
        int sentButtons = -1, sentLx, sentLy, sentRx, sentRy;
        boolean sentActive;

        OverlayView(Context c) {
            super(c);
            text.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
            text.setTextAlign(Paint.Align.CENTER);
            textDim.setTextAlign(Paint.Align.LEFT);
            textDim.setTypeface(Typeface.MONOSPACE);
            textDim.setTextSize(11 * dp);
            setHapticFeedbackEnabled(true);
        }

        void resetPointers() {
            captured.clear();
            points.clear();
            for (Ctl k : ctls) k.reset();
            update();
        }

        /** The game picture: 16:9, as tall as it fits, centred (the presenter letterboxes). */
        RectF picture() {
            float w = getWidth(), h = getHeight();
            float pw = Math.min(w, h * 16f / 9f), ph = pw * 9f / 16f;
            return new RectF((w - pw) / 2, (h - ph) / 2, (w + pw) / 2, (h + ph) / 2);
        }

        void build() {
            builtFor = controls * 2 + (ps3 ? 1 : 0);
            builtW = getWidth();
            builtH = getHeight();
            ctls.clear();
            RectF pic = picture();
            float w = getWidth(), h = getHeight();
            float margin = pic.left;
            boolean roomy = margin >= 44 * dp;
            // Strip buttons: in the margins when there is room, else in the picture's top corners.
            float s = roomy ? Math.min(margin * 0.62f, 46 * dp) : 36 * dp;
            float pad = 8 * dp;
            float leftCx = roomy ? margin / 2 : pic.left + pad + s / 2;
            float rightCx = roomy ? w - margin / 2 : pic.right - pad - s / 2;
            ark.set(leftCx - s / 2, pad, leftCx + s / 2, pad + s);
            camera.set(rightCx - s / 2, pad, rightCx + s / 2, pad + s);
            readoutX = roomy ? 4 * dp : pic.left + pad;
            readoutY = ark.bottom + 16 * dp;
            graph.set(readoutX, readoutY + 20 * dp, roomy ? margin - 4 * dp : readoutX + 72 * dp, readoutY + 40 * dp);
            // Back / Start: small, at the top centre of the picture (where they sit on a controller).
            float ph = 24 * dp, pill = 58 * dp;
            if (controls == CONTROLS_OFF) return;
            ctls.add(Ctl.button(BACK, pic.centerX() - pill * 0.62f, pic.top + pad + ph / 2, pill, ph, Ctl.PILL));
            ctls.add(Ctl.button(START, pic.centerX() + pill * 0.62f, pic.top + pad + ph / 2, pill, ph, Ctl.PILL));
            // Two wheels, one per thumb, in the bottom corners of the screen (over the picture):
            // left: the stick, with the d-pad, L3, LT and LB on an arc around it;
            // right: the face buttons, with RB, RT, R3 and the right stick on an arc around them.
            // A thumb resting on the stick or the face buttons reaches the arc by rotating.
            float u = h * 0.1f * SIZE_SCALE[Math.max(0, Math.min(2, touchSize))];
            // Wheel centres a little below the middle of the screen, where the thumbs rest when the
            // phone is held sideways (not down at the bottom edge).
            float lcx = 2.35f * u, rcx = w - 2.35f * u, cy = h * 0.55f;
            float arc = 2.45f * u, slot = 1.0f * u;
            // Angles: 0 = right, 90 = up (the right wheel mirrors the left: 180 - angle).
            // Xbox: left stick at the left thumb's resting place, d-pad below and inside it;
            // DualShock: d-pad at the resting place, left stick below and inside it. Right: face
            // buttons at the resting place, right stick below and inside (both controllers).
            float innerArc = arc + 0.35f * u;
            float[] lowIn = polar(lcx, cy, -22, innerArc);
            if (ps3) {
                ctls.add(Ctl.dpad(lcx, cy, 1.9f * u));
            } else {
                ctls.add(Ctl.dpad(lowIn[0], lowIn[1], 1.5f * u));
            }
            float[] p;
            p = polar(lcx, cy, 42, arc); ctls.add(Ctl.button(L3, p[0], p[1], slot * 0.85f, slot * 0.85f, Ctl.ROUND));
            p = polar(lcx, cy, 86, arc); ctls.add(Ctl.button(LB, p[0], p[1], slot * 1.5f, slot * 0.8f, Ctl.PILL));
            p = polar(lcx, cy, 122, arc); ctls.add(Ctl.button(LT, p[0], p[1], slot * 1.5f, slot * 0.8f, Ctl.PILL));
            p = polar(rcx, cy, 180 - 42, arc); ctls.add(Ctl.button(R3, p[0], p[1], slot * 0.85f, slot * 0.85f, Ctl.ROUND));
            p = polar(rcx, cy, 180 - 86, arc); ctls.add(Ctl.button(RB, p[0], p[1], slot * 1.5f, slot * 0.8f, Ctl.PILL));
            p = polar(rcx, cy, 180 - 122, arc); ctls.add(Ctl.button(RT, p[0], p[1], slot * 1.5f, slot * 0.8f, Ctl.PILL));
            addFace(rcx, cy, 2.3f * u);
            float[] rs = polar(rcx, cy, 180 + 22, innerArc);
            ctls.add(Ctl.stick(false, rs[0], rs[1], 2.1f * u, new RectF(rs[0] - 1.5f * u, rs[1] - 1.5f * u, rs[0] + 1.5f * u, rs[1] + 1.6f * u)));
            // Left stick: floats - wherever the thumb lands in the lower left (the other controls
            // there win where they are); it rests at its controller's place.
            float[] ls = ps3 ? lowIn : new float[] {lcx, cy};
            ctls.add(Ctl.stick(true, ls[0], ls[1], ps3 ? 2.1f * u : 2.3f * u, new RectF(0, h * 0.25f, w * 0.42f, h)));
        }

        float[] polar(float cx, float cy, float degrees, float r) {
            double a = Math.toRadians(degrees);
            return new float[] {cx + (float) Math.cos(a) * r, cy - (float) Math.sin(a) * r};
        }

        /** The face buttons; a touch anywhere in the cluster presses the nearest one. */
        void addFace(float cx, float cy, float u) {
            float b = u * 0.4f, o = u * 0.3f;
            int[][] f = {{A, 0, 1}, {B, 1, 0}, {LrTouch.X, -1, 0}, {LrTouch.Y, 0, -1}};
            for (int[] k : f) {
                Ctl c = Ctl.button(k[0], cx + k[1] * o, cy + k[2] * o, b, b, Ctl.ROUND);
                c.clusterX = cx;
                c.clusterY = cy;
                c.clusterR = u * 0.62f;
                ctls.add(c);
            }
        }

        String label(int bit) {
            switch (bit) {
                case A: return ps3 ? "✕" : "A";
                case B: return ps3 ? "○" : "B";
                case LrTouch.X: return ps3 ? "□" : "X";
                case LrTouch.Y: return ps3 ? "△" : "Y";
                case LB: return ps3 ? "L1" : "LB";
                case RB: return ps3 ? "R1" : "RB";
                case LT: return ps3 ? "L2" : "LT";
                case RT: return ps3 ? "R2" : "RT";
                case BACK: return ps3 ? "SELECT" : "BACK";
                case START: return "START";
                default: return "";
            }
        }

        final Paint loading = paint(IVORY, Paint.Style.FILL, 0);

        /** Until the game's first frame: LOADING in wide serif capitals under a turning clock ring. */
        void drawLoading(Canvas c) {
            float cx = getWidth() / 2f, cy = getHeight() / 2f;
            loading.setTypeface(Typeface.create("serif", Typeface.NORMAL));
            loading.setTextAlign(Paint.Align.CENTER);
            loading.setTextSize(20 * dp);
            loading.setLetterSpacing(0.6f);
            long t = System.currentTimeMillis() - loadingStart;
            int a = (int) (170 + 85 * Math.sin(t / 600.0));
            loading.setAlpha(a);
            c.drawText("LOADING", cx + 6 * dp, cy + 46 * dp, loading);
            float r = 22 * dp;
            Paint ring = paint(GOLD, Paint.Style.STROKE, 1.4f * dp);
            c.drawCircle(cx, cy, r, ring);
            for (int i = 0; i < 12; ++i) {
                double ang = Math.PI * 2 * i / 12;
                float in = i % 3 == 0 ? r * 0.72f : r * 0.84f;
                c.drawLine(cx + (float) Math.sin(ang) * in, cy - (float) Math.cos(ang) * in,
                        cx + (float) Math.sin(ang) * r * 0.95f, cy - (float) Math.cos(ang) * r * 0.95f, ring);
            }
            double hand = t / 1000.0 * Math.PI;  // one turn every two seconds
            c.drawLine(cx, cy, cx + (float) Math.sin(hand) * r * 0.7f, cy - (float) Math.cos(hand) * r * 0.7f, ring);
            double small = t / 12000.0 * Math.PI;
            c.drawLine(cx, cy, cx + (float) Math.sin(small) * r * 0.45f, cy - (float) Math.cos(small) * r * 0.45f, ring);
        }

        @Override
        protected void onDraw(Canvas c) {
            if (!gameStarted) {
                drawLoading(c);
                return;
            }
            if (builtFor != controls * 2 + (ps3 ? 1 : 0) || builtW != getWidth() || builtH != getHeight()) build();
            boolean menuOpen = menu.getVisibility() == View.VISIBLE;
            // Ark button.
            c.drawRoundRect(ark, ark.width() / 2, ark.height() / 2, menuOpen ? pressed : glass);
            c.drawRoundRect(ark, ark.width() / 2, ark.height() / 2, gold);
            drawRing(c, ark.centerX(), ark.centerY(), ark.width() * 0.3f, gold);
            // Camera button: an aperture.
            c.drawRoundRect(camera, camera.width() * 0.3f, camera.height() * 0.3f, cameraDown ? pressed : glass);
            c.drawRoundRect(camera, camera.width() * 0.3f, camera.height() * 0.3f, gold);
            c.drawCircle(camera.centerX(), camera.centerY(), camera.width() * 0.22f, gold);
            c.drawCircle(camera.centerX(), camera.centerY(), camera.width() * 0.08f, goldDim);
            // Readout.
            if (readout != READOUT_OFF) {
                float y = readoutY;
                textDim.setColor(stats[0] > 0 && stats[0] < 27 ? 0xFFE0A070 : IVORY_DIM);
                c.drawText(String.format(Locale.US, "%.1f fps", stats[0]), readoutX, y, textDim);
                if (readout == READOUT_FULL) {
                    c.drawText(String.format(Locale.US, "%.1f ms", stats[1]), readoutX, y + 14 * dp, textDim);
                    drawGraph(c);  // slow frames show in orange there
                }
            }
            if (menuOpen) return;
            if (hiddenByPad) {
                // Back / Start stay; the rest waits for a touch.
                for (Ctl k : ctls) if (k.bit == BACK || k.bit == START) k.draw(this, c, 255);
                return;
            }
            for (Ctl k : ctls) k.draw(this, c, k.bit == BACK || k.bit == START ? 255 : 165);
        }

        boolean cameraDown;
        final Paint bar = paint(0xB0EDE6D6, Paint.Style.FILL, 0);
        final Paint barSlow = paint(0xFFE0A070, Paint.Style.FILL, 0);
        final Paint guide = paint(0x80C2A66B, Paint.Style.STROKE, 1f);

        /** The last 120 frame times as bars, 0-50 ms; gold lines at 16.7 and 33.3 ms. */
        void drawGraph(Canvas c) {
            float[] f = frameTimes;
            if (f.length == 0 || graph.width() <= 0) return;
            c.drawRoundRect(graph, 3 * dp, 3 * dp, glass);
            float bw = graph.width() / f.length, scale = graph.height() / 50f;
            for (int i = 0; i < f.length; ++i) {
                float ms = Math.min(f[i], 50f);
                if (ms <= 0) continue;
                float x = graph.left + i * bw;
                c.drawRect(x, graph.bottom - ms * scale, x + Math.max(bw, 1f), graph.bottom, f[i] > 40 ? barSlow : bar);
            }
            for (float ms : new float[] {16.7f, 33.3f}) {
                float y = graph.bottom - ms * scale;
                c.drawLine(graph.left, y, graph.right, y, guide);
            }
        }

        @Override
        public boolean onTouchEvent(MotionEvent e) {
            if (!gameStarted) return false;
            if (builtFor != controls * 2 + (ps3 ? 1 : 0) || builtW != getWidth() || builtH != getHeight()) build();
            int action = e.getActionMasked();
            int index = e.getActionIndex();
            int id = e.getPointerId(index);
            if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
                float x = e.getX(index), y = e.getY(index);
                if (grow(ark, 6 * dp).contains(x, y)) {
                    captured.put(id, -2);
                    performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                    if (menu.getVisibility() == View.VISIBLE) closeMenu(); else openMenu();
                    invalidate();
                    return true;
                }
                if (grow(camera, 6 * dp).contains(x, y)) {
                    captured.put(id, -3);
                    cameraDown = true;
                    performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                    screenshot();
                    invalidate();
                    return true;
                }
                if (hiddenByPad && y > getHeight() * 0.35f) {
                    // Touching where the controls were brings them back.
                    hiddenByPad = false;
                    invalidate();
                    return true;
                }
                int hit = hitIndex(x, y, false);
                if (hit < 0) return action == MotionEvent.ACTION_POINTER_DOWN;  // not ours: the game gets it
                captured.put(id, hit);
                points.put(id, new float[] {x, y, e.getEventTime()});
                ctls.get(hit).begin(id, x, y, e.getEventTime());
                performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
            } else if (action == MotionEvent.ACTION_MOVE) {
                for (int i = 0; i < e.getPointerCount(); ++i) {
                    int pid = e.getPointerId(i);
                    if (captured.indexOfKey(pid) < 0) continue;
                    points.put(pid, new float[] {e.getX(i), e.getY(i), e.getEventTime()});
                }
            } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP
                    || action == MotionEvent.ACTION_CANCEL) {
                if (action == MotionEvent.ACTION_CANCEL) {
                    resetPointers();
                    cameraDown = false;
                    invalidate();
                    return true;
                }
                int k = captured.get(id, -1);
                if (k == -3) cameraDown = false;
                if (k >= 0 && k < ctls.size()) ctls.get(k).end(id, e.getEventTime());
                captured.delete(id);
                points.delete(id);
            }
            update();
            invalidate();
            return true;
        }

        /** The control under a point; in the face cluster the nearest button. Buttons only when sliding. */
        int hitIndex(float x, float y, boolean buttonsOnly) {
            int best = -1, other = -1;
            float bestD = Float.MAX_VALUE;
            for (int i = 0; i < ctls.size(); ++i) {
                Ctl k = ctls.get(i);
                if (buttonsOnly && (k.kind == Ctl.STICK || k.kind == Ctl.DPAD)) continue;
                if (!k.hits(x, y, dp)) continue;
                if (k.kind != Ctl.ROUND || k.clusterR <= 0) {
                    if (other < 0) other = i;
                    continue;
                }
                float d = (x - k.cx) * (x - k.cx) + (y - k.cy) * (y - k.cy);
                if (d < bestD) {
                    bestD = d;
                    best = i;
                }
            }
            return best >= 0 ? best : other;  // the face cluster wins where areas overlap
        }

        RectF grow(RectF r, float by) {
            return new RectF(r.left - by, r.top - by, r.right + by, r.bottom + by);
        }

        /** Recomputes the pad from every finger on the controls and sends it to the game. */
        void update() {
            int buttons = 0;
            float[] sticks = {0, 0, 0, 0};
            for (Ctl k : ctls) k.held = false;
            for (int i = 0; i < captured.size(); ++i) {
                int pid = captured.keyAt(i), idx = captured.valueAt(i);
                float[] p = points.get(pid);
                if (idx < 0 || idx >= ctls.size() || p == null) continue;
                Ctl k = ctls.get(idx);
                if (k.kind == Ctl.STICK || k.kind == Ctl.DPAD) {
                    k.move(p[0], p[1]);
                } else {
                    // A finger sliding off a button onto another presses that one.
                    int u = hitIndex(p[0], p[1], true);
                    if (u >= 0) ctls.get(u).held = true;
                }
            }
            for (Ctl k : ctls) {
                buttons |= k.bits();
                if (k.kind == Ctl.STICK) {
                    int o = k.left ? 0 : 2;
                    sticks[o] = k.vx;
                    sticks[o + 1] = k.vy;
                }
            }
            boolean active = controls != CONTROLS_OFF && menu.getVisibility() != View.VISIBLE;
            int lx = (int) (sticks[0] * 32767), ly = (int) (-sticks[1] * 32767);
            int rx = (int) (sticks[2] * 32767), ry = (int) (-sticks[3] * 32767);
            if (active != sentActive || buttons != sentButtons || lx != sentLx || ly != sentLy || rx != sentRx || ry != sentRy) {
                sentActive = active;
                sentButtons = buttons;
                sentLx = lx;
                sentLy = ly;
                sentRx = rx;
                sentRy = ry;
                sendPad(active, buttons, lx, ly, rx, ry);
            }
            // A stick click lasts a moment after the tap.
            for (Ctl k : ctls) if (k.clickUntil > 0) { postDelayed(this::update, 130); break; }
        }
    }

    /** One on-screen control. */
    static final class Ctl {
        static final int PILL = 0, ROUND = 1, STICK = 2, DPAD = 3;
        final int kind;
        final int bit;
        final float w, h;
        float cx, cy;          // a stick's centre follows the thumb (see begin)
        final float homeX, homeY;
        final boolean left;
        RectF zone;            // stick: where a touch grabs it
        float clusterX, clusterY, clusterR;  // face buttons: the cluster they share
        boolean held;
        float vx, vy;          // stick deflection, -1..1 (y down)
        int dpadBits;
        int pointer = -1;
        float downX, downY;
        long downTime;
        long clickUntil;
        long lastTap;

        private Ctl(int kind, int bit, float cx, float cy, float w, float h, boolean left) {
            this.kind = kind;
            this.bit = bit;
            this.cx = cx;
            this.cy = cy;
            this.homeX = cx;
            this.homeY = cy;
            this.w = w;
            this.h = h;
            this.left = left;
        }

        static Ctl button(int bit, float cx, float cy, float w, float h, int shape) {
            return new Ctl(shape, bit, cx, cy, w, h, false);
        }

        static Ctl stick(boolean left, float cx, float cy, float d, RectF zone) {
            Ctl c = new Ctl(STICK, left ? L3 : R3, cx, cy, d, d, left);
            c.zone = zone;
            return c;
        }

        static Ctl dpad(float cx, float cy, float d) {
            return new Ctl(DPAD, 0, cx, cy, d, d, false);
        }

        boolean hits(float x, float y, float dp) {
            if (kind == STICK && zone != null && zone.contains(x, y)) return true;
            if (kind == ROUND && clusterR > 0) {
                return (x - clusterX) * (x - clusterX) + (y - clusterY) * (y - clusterY) <= clusterR * clusterR;
            }
            float slack = kind == STICK ? w * 0.2f : 4 * dp;
            if (kind == ROUND || kind == STICK || kind == DPAD) {
                float r = w / 2 + slack;
                return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
            }
            return Math.abs(x - cx) <= w / 2 + slack && Math.abs(y - cy) <= h / 2 + slack;
        }

        void reset() {
            held = false;
            vx = vy = 0;
            dpadBits = 0;
            pointer = -1;
            cx = homeX;
            cy = homeY;
        }

        void begin(int id, float x, float y, long t) {
            pointer = id;
            downX = x;
            downY = y;
            downTime = t;
            if (kind == STICK && zone != null) {
                // The stick centres where the thumb lands (kept inside its area).
                float r = w / 2;
                cx = Math.max(zone.left + r * 0.6f, Math.min(zone.right - r * 0.6f, x));
                cy = Math.max(zone.top + r * 0.6f, Math.min(zone.bottom - r * 0.6f, y));
                downX = cx;
                downY = cy;
            }
            move(x, y);
        }

        void end(int id, long t) {
            if (id != pointer) return;
            if (kind == STICK) {
                // A double tap is a stick click (one tap just grabs the stick where the thumb is).
                boolean tap = t - downTime < 220 && Math.hypot(vx, vy) < 0.35f;
                if (tap && t - lastTap < 350) {
                    clickUntil = System.currentTimeMillis() + 120;
                    lastTap = 0;
                } else {
                    lastTap = tap ? t : 0;
                }
            }
            reset();
        }

        void move(float x, float y) {
            if (kind == STICK) {
                float r = w / 2;
                float dx = (x - cx) / r, dy = (y - cy) / r;
                float m = (float) Math.hypot(dx, dy);
                if (m > 1) {
                    dx /= m;
                    dy /= m;
                }
                vx = dx;
                vy = dy;
            } else if (kind == DPAD) {
                float dx = x - cx, dy = y - cy;
                dpadBits = 0;
                if (Math.hypot(dx, dy) < w * 0.12f) return;
                double a = Math.toDegrees(Math.atan2(-dy, dx));  // 0 = right, 90 = up
                if (a < 0) a += 360;
                if (a < 67.5 || a > 292.5) dpadBits |= RIGHT;
                if (a > 22.5 && a < 157.5) dpadBits |= UP;
                if (a > 112.5 && a < 247.5) dpadBits |= LEFT;
                if (a > 202.5 && a < 337.5) dpadBits |= DOWN;
            }
        }

        int bits() {
            switch (kind) {
                case STICK:
                    if (clickUntil > 0) {
                        if (System.currentTimeMillis() < clickUntil) return bit;
                        clickUntil = 0;
                    }
                    return 0;
                case DPAD: return pointer >= 0 ? dpadBits : 0;
                default: return held ? bit : 0;
            }
        }

        void draw(OverlayView v, Canvas c, int alpha) {
            Paint fill = held || (kind == DPAD && dpadBits != 0) ? v.pressed : v.glass;
            int fa = fill.getAlpha(), ga = v.gold.getAlpha(), ta = v.text.getAlpha();
            fill.setAlpha(fa * alpha / 255);
            v.gold.setAlpha(ga * alpha / 255);
            v.text.setAlpha(ta * alpha / 255);
            switch (kind) {
                case PILL: {
                    RectF r = new RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
                    c.drawRoundRect(r, h / 2, h / 2, fill);
                    c.drawRoundRect(r, h / 2, h / 2, v.gold);
                    v.text.setTextSize(Math.min(h * 0.46f, w * 0.2f));
                    c.drawText(v.label(bit), cx, cy + v.text.getTextSize() * 0.36f, v.text);
                    break;
                }
                case ROUND: {
                    c.drawCircle(cx, cy, w / 2, fill);
                    c.drawCircle(cx, cy, w / 2, v.gold);
                    v.text.setTextSize(w * 0.46f);
                    c.drawText(v.label(bit), cx, cy + v.text.getTextSize() * 0.36f, v.text);
                    break;
                }
                case STICK: {
                    c.drawCircle(cx, cy, w / 2, fill);
                    c.drawCircle(cx, cy, w / 2, v.gold);
                    c.drawCircle(cx, cy, w * 0.33f, v.goldDim);
                    int ka = v.knob.getAlpha();
                    v.knob.setAlpha(ka * alpha / 255);
                    c.drawCircle(cx + vx * w * 0.3f, cy + vy * w * 0.3f, w * 0.2f, v.knob);
                    v.knob.setAlpha(ka);
                    break;
                }
                case DPAD: {
                    float a = w * 0.17f, r = w / 2;
                    android.graphics.Path p = new android.graphics.Path();
                    p.moveTo(cx - a, cy - r); p.lineTo(cx + a, cy - r); p.lineTo(cx + a, cy - a);
                    p.lineTo(cx + r, cy - a); p.lineTo(cx + r, cy + a); p.lineTo(cx + a, cy + a);
                    p.lineTo(cx + a, cy + r); p.lineTo(cx - a, cy + r); p.lineTo(cx - a, cy + a);
                    p.lineTo(cx - r, cy + a); p.lineTo(cx - r, cy - a); p.lineTo(cx - a, cy - a);
                    p.close();
                    c.drawPath(p, v.glass);
                    if (dpadBits != 0) {
                        float o = r * 0.62f;
                        if ((dpadBits & UP) != 0) c.drawCircle(cx, cy - o, a * 0.8f, v.pressed);
                        if ((dpadBits & DOWN) != 0) c.drawCircle(cx, cy + o, a * 0.8f, v.pressed);
                        if ((dpadBits & LEFT) != 0) c.drawCircle(cx - o, cy, a * 0.8f, v.pressed);
                        if ((dpadBits & RIGHT) != 0) c.drawCircle(cx + o, cy, a * 0.8f, v.pressed);
                    }
                    c.drawPath(p, v.gold);
                    break;
                }
                default:
                    break;
            }
            fill.setAlpha(fa);
            v.gold.setAlpha(ga);
            v.text.setAlpha(ta);
        }
    }

    // ---------------------------------------------------------------------------------------
    // The Ark menu: a panel over the picture; touch, or a controller (up / down, A, B).
    // ---------------------------------------------------------------------------------------
    final class ArkMenu extends FrameLayout {
        final Paint shade = paint(SHADE, Paint.Style.FILL, 0);
        final Paint glass = paint(0xF2101216, Paint.Style.FILL, 0);
        final Paint gold = paint(GOLD, Paint.Style.STROKE, 1.2f * dp);
        final Paint rowSel = paint(0x553A3220, Paint.Style.FILL, 0);
        final Paint title = paint(GOLD, Paint.Style.FILL, 0);
        final Paint item = paint(IVORY, Paint.Style.FILL, 0);
        final Paint value = paint(GOLD, Paint.Style.FILL, 0);
        final Paint hint = paint(IVORY_DIM, Paint.Style.FILL, 0);
        final List<String[]> rows = new ArrayList<>();
        final RectF panel = new RectF();
        float rowH, rowsTop;
        int selected;
        boolean quitArmed;
        static final int ROW_QUIT = 8;
        String statusText = "";

        ArkMenu(Context c) {
            super(c);
            setWillNotDraw(false);
            title.setTextSize(17 * dp);
            title.setLetterSpacing(0.35f);
            title.setTypeface(Typeface.create("serif", Typeface.NORMAL));
            item.setTextSize(15 * dp);
            value.setTextSize(13 * dp);
            value.setTextAlign(Paint.Align.RIGHT);
            hint.setTextSize(12 * dp);
            setClickable(true);  // the game behind gets no touches while it is open
        }

        void open() {
            selected = 0;
            quitArmed = false;
            statusText = "";
            setVisibility(View.VISIBLE);
            invalidate();
            overlay.invalidate();
        }

        void status(String s) {
            statusText = s;
            invalidate();
        }

        void buildRows() {
            rows.clear();
            rows.add(new String[] {"Export saves", "to Downloads"});
            rows.add(new String[] {"Export saves to", "Drive, any folder"});
            rows.add(new String[] {"Import saves", "restarts the game"});
            rows.add(new String[] {"Backup", backupName(backup)});
            rows.add(new String[] {"Touch controls", controlsName(controls)});
            rows.add(new String[] {"Control size", sizeName(touchSize)});
            rows.add(new String[] {"Readout", readoutName(readout)});
            rows.add(new String[] {"Screenshot", ""});
            rows.add(new String[] {quitArmed ? "Press again" : "Quit game", quitArmed ? "progress is lost" : ""});
        }

        static final int COLS = 3, CLOSE = 9;
        final RectF[] tiles = new RectF[9];
        final RectF closeButton = new RectF();
        final Paint tileFill = paint(0xFF1A1C22, Paint.Style.FILL, 0);
        final Paint tileSel = paint(0xFF3A3220, Paint.Style.FILL, 0);
        final Paint tileEdge = paint(0x99C9A45C, Paint.Style.STROKE, 1 * dp);
        final Paint icon = paint(GOLD, Paint.Style.STROKE, 2.2f * dp);
        final Path path = new Path();

        @Override
        protected void onDraw(Canvas c) {
            buildRows();
            float w = getWidth(), h = getHeight();
            c.drawRect(0, 0, w, h, shade);
            float gap = 10 * dp, head = 54 * dp, foot = 30 * dp;
            float tileW = Math.min(180 * dp, (w * 0.74f - 2 * gap - 32 * dp) / COLS);
            float tile = Math.min(tileW * 0.75f, (h - 2 * gap - head - foot - 24 * dp) / 3);  // height
            float pw = tileW * COLS + gap * (COLS - 1) + 32 * dp, ph = head + tile * 3 + gap * 2 + foot;
            panel.set((w - pw) / 2, (h - ph) / 2, (w + pw) / 2, (h + ph) / 2);
            c.drawRoundRect(panel, 16 * dp, 16 * dp, glass);
            c.drawRoundRect(panel, 16 * dp, 16 * dp, gold);
            drawRing(c, panel.left + 30 * dp, panel.top + 28 * dp, 11 * dp, gold);
            c.drawText("ARK", panel.left + 50 * dp, panel.top + 34 * dp, title);
            hint.setTextAlign(Paint.Align.RIGHT);
            c.drawText(ps3 ? "✕ select   ○ close" : "A select   B close", panel.right - 58 * dp, panel.top + 33 * dp, hint);
            // Close button (top right).
            float cb = 30 * dp;
            closeButton.set(panel.right - 14 * dp - cb, panel.top + 12 * dp, panel.right - 14 * dp, panel.top + 12 * dp + cb);
            c.drawRoundRect(closeButton, cb / 2, cb / 2, selected == CLOSE ? tileSel : tileFill);
            c.drawRoundRect(closeButton, cb / 2, cb / 2, tileEdge);
            float q = cb * 0.22f;
            c.drawLine(closeButton.centerX() - q, closeButton.centerY() - q, closeButton.centerX() + q, closeButton.centerY() + q, icon);
            c.drawLine(closeButton.centerX() + q, closeButton.centerY() - q, closeButton.centerX() - q, closeButton.centerY() + q, icon);
            float gridLeft = panel.left + 16 * dp, gridTop = panel.top + head;
            item.setTextAlign(Paint.Align.CENTER);
            value.setTextAlign(Paint.Align.CENTER);
            item.setTextSize(Math.min(15 * dp, tile * 0.15f));
            value.setTextSize(Math.min(12.5f * dp, tile * 0.12f));
            for (int i = 0; i < 9; ++i) {
                float l = gridLeft + (i % COLS) * (tileW + gap), t = gridTop + (i / COLS) * (tile + gap);
                if (tiles[i] == null) tiles[i] = new RectF();
                tiles[i].set(l, t, l + tileW, t + tile);
                boolean quit = i == ROW_QUIT && quitArmed;
                c.drawRoundRect(tiles[i], 12 * dp, 12 * dp, i == selected ? tileSel : tileFill);
                c.drawRoundRect(tiles[i], 12 * dp, 12 * dp, i == selected ? gold : tileEdge);
                icon.setColor(quit ? 0xFFE8907A : GOLD);
                drawIcon(c, i, tiles[i].centerX(), t + tile * 0.33f, tile * 0.16f);
                item.setColor(quit ? 0xFFE8907A : IVORY);
                c.drawText(rows.get(i)[0], tiles[i].centerX(), t + tile * 0.71f, item);
                c.drawText(rows.get(i)[1], tiles[i].centerX(), t + tile * 0.86f, value);
            }
            icon.setColor(GOLD);
            item.setTextAlign(Paint.Align.LEFT);
            value.setTextAlign(Paint.Align.RIGHT);
            hint.setTextAlign(Paint.Align.LEFT);
            if (!statusText.isEmpty()) c.drawText(statusText, panel.left + 22 * dp, panel.bottom - 11 * dp, hint);
        }

        // Line icons, r = half size, centred on (x, y).
        void drawIcon(Canvas c, int i, float x, float y, float r) {
            path.reset();
            switch (i) {
                case 0:  // export: arrow up out of a tray
                case 1:  // export to: the same, with a folder tab
                case 2: {  // import: arrow down into a tray
                    path.moveTo(x - r, y + r * 0.2f); path.lineTo(x - r, y + r); path.lineTo(x + r, y + r); path.lineTo(x + r, y + r * 0.2f);
                    c.drawPath(path, icon);
                    boolean up = i != 2;
                    float a0 = up ? y + r * 0.45f : y - r, a1 = up ? y - r : y + r * 0.45f;
                    c.drawLine(x, a0, x, a1, icon);
                    float tip = a1, dir = up ? 1 : -1;
                    c.drawLine(x, tip, x - r * 0.45f, tip + dir * r * 0.45f, icon);
                    c.drawLine(x, tip, x + r * 0.45f, tip + dir * r * 0.45f, icon);
                    if (i == 1) c.drawLine(x + r * 0.45f, y - r * 0.9f, x + r * 1.15f, y - r * 0.9f, icon);
                    break;
                }
                case 3: {  // backup: circular arrow
                    RectF o = new RectF(x - r, y - r, x + r, y + r);
                    c.drawArc(o, -60, 300, false, icon);
                    float ex = x + r * (float) Math.cos(Math.toRadians(-60)), ey = y + r * (float) Math.sin(Math.toRadians(-60));
                    c.drawLine(ex, ey, ex - r * 0.5f, ey - r * 0.05f, icon);
                    c.drawLine(ex, ey, ex + r * 0.05f, ey + r * 0.5f, icon);
                    c.drawLine(x, y - r * 0.5f, x, y, icon);
                    c.drawLine(x, y, x + r * 0.4f, y + r * 0.3f, icon);
                    break;
                }
                case 4: {  // touch controls: two thumb circles
                    c.drawCircle(x - r * 0.55f, y, r * 0.45f, icon);
                    c.drawCircle(x + r * 0.55f, y, r * 0.45f, icon);
                    c.drawCircle(x - r * 0.55f, y, r * 0.12f, icon);
                    c.drawCircle(x + r * 0.55f, y, r * 0.12f, icon);
                    break;
                }
                case 5: {  // size: small and large square
                    c.drawRoundRect(new RectF(x - r, y - r * 0.1f, x - r * 0.1f, y + r * 0.8f), 3 * dp, 3 * dp, icon);
                    c.drawRoundRect(new RectF(x - r * 0.25f, y - r, x + r, y + r * 0.8f), 3 * dp, 3 * dp, icon);
                    break;
                }
                case 6: {  // readout: bars
                    for (int k = 0; k < 4; ++k) {
                        float bx = x - r + k * r * 0.62f + r * 0.1f, bh = r * (0.6f + 0.45f * ((k * 7) % 4) / 3f);
                        c.drawLine(bx, y + r, bx, y + r - bh, icon);
                    }
                    c.drawLine(x - r * 1.1f, y + r, x + r * 1.1f, y + r, icon);
                    break;
                }
                case 7: {  // screenshot: camera
                    c.drawRoundRect(new RectF(x - r, y - r * 0.55f, x + r, y + r * 0.8f), 4 * dp, 4 * dp, icon);
                    c.drawCircle(x, y + r * 0.12f, r * 0.38f, icon);
                    c.drawLine(x - r * 0.4f, y - r * 0.55f, x - r * 0.25f, y - r * 0.85f, icon);
                    c.drawLine(x - r * 0.25f, y - r * 0.85f, x + r * 0.25f, y - r * 0.85f, icon);
                    c.drawLine(x + r * 0.25f, y - r * 0.85f, x + r * 0.4f, y - r * 0.55f, icon);
                    break;
                }
                default: {  // quit: power symbol
                    c.drawArc(new RectF(x - r, y - r, x + r, y + r), -55, 290, false, icon);
                    c.drawLine(x, y - r * 1.15f, x, y - r * 0.15f, icon);
                    break;
                }
            }
        }

        @Override
        public boolean onTouchEvent(MotionEvent e) {
            if (e.getActionMasked() != MotionEvent.ACTION_UP) return true;
            float x = e.getX(), y = e.getY();
            RectF closeHit = new RectF(closeButton.left - 8 * dp, closeButton.top - 8 * dp, closeButton.right + 8 * dp, closeButton.bottom + 8 * dp);
            if (!panel.contains(x, y) || closeHit.contains(x, y)) {
                closeMenu();
                overlay.invalidate();
                return true;
            }
            for (int i = 0; i < 9; ++i) {
                if (tiles[i] != null && tiles[i].contains(x, y)) {
                    selected = i;
                    choose(i);
                    break;
                }
            }
            return true;
        }

        void onPad(int pressed) {
            if (selected == CLOSE) {
                if ((pressed & DOWN) != 0) selected = COLS - 1;
                else if ((pressed & LEFT) != 0) selected = COLS - 1;
            } else {
                int col = selected % COLS, row = selected / COLS;
                if ((pressed & UP) != 0) selected = row == 0 ? CLOSE : selected - COLS;
                else if ((pressed & DOWN) != 0) selected = row == 2 ? col : selected + COLS;
                else if ((pressed & LEFT) != 0) selected = row * COLS + (col + COLS - 1) % COLS;
                else if ((pressed & RIGHT) != 0) selected = row * COLS + (col + 1) % COLS;
            }
            if ((pressed & A) != 0) {
                if (selected == CLOSE) {
                    closeMenu();
                    overlay.invalidate();
                    return;
                }
                choose(selected);
            }
            if ((pressed & (B | START)) != 0) {
                closeMenu();
                overlay.invalidate();
                return;
            }
            invalidate();
        }

        void choose(int i) {
            performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
            if (i != ROW_QUIT) quitArmed = false;
            switch (i) {
                case 0: runInBackground(LrTouch.this::exportToDownloads); break;
                case 1: pickExport(); break;
                case 2: pickImport(); break;
                case 3:
                    backup = (backup + 1) % 3;
                    prefs.edit().putInt("backup", backup).apply();
                    status(backup == BACKUP_OFF ? "No automatic backups" : "A backup is written after each save");
                    break;
                case 4: setControls(controls == CONTROLS_ON ? CONTROLS_OFF : CONTROLS_ON); break;
                case 5: setTouchSize((touchSize + 1) % 3); break;
                case 6: setReadout((readout + 1) % 3); break;
                case 7:
                    closeMenu();
                    overlay.invalidate();
                    // After the menu is gone from the screen.
                    handler.postDelayed(LrTouch.this::screenshot, 250);
                    return;
                case ROW_QUIT:
                    if (!quitArmed) {
                        quitArmed = true;
                        status("Press Quit game again to close the game");
                    } else {
                        activity.finishAffinity();
                        Process.killProcess(Process.myPid());
                    }
                    break;
                default:
                    closeMenu();
                    overlay.invalidate();
                    return;
            }
            invalidate();
        }
    }
}
