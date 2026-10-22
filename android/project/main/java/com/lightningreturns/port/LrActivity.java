package com.lightningreturns.port;

import android.content.Intent;
import android.os.Bundle;
import android.os.Process;
import android.util.Log;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.util.ArrayList;

/**
 * The game. SDL3 (inside librexruntime.so) drives the window, input (touch, keyboard,
 * gamepads) and audio; libmain.so's SDL_main runs the recompiled game.
 */
public class LrActivity extends SDLActivity {
    static final String TAG = "LightningReturns";
    static final String EXTRA_GAME_ROOT = "game_root";
    static final String EXTRA_USER_ROOT = "user_root";
    static final String EXTRA_GENERATION = "relaunch_generation";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Logged by the game at startup ("LR android build: ..."), so a log shows which APK ran.
        try {
            android.system.Os.setenv("LR_APK_VERSION",
                    getPackageManager().getPackageInfo(getPackageName(), 0).versionName, true);
        } catch (Exception e) {
            Log.w(TAG, "cannot read the app version", e);
        }
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        requestHighRefreshRate();
        // The touch layer over the game (Ark menu, on-screen controls, readout): LrTouch.java.
        if (getContentView() != null) {
            Intent intent = getIntent();
            String user = intent != null ? intent.getStringExtra(EXTRA_USER_ROOT) : null;
            LrTouch.attach(this, user != null ? user : LrPaths.userRoot(this));
        }
    }

    /**
     * Runs the screen at its highest refresh rate (120 Hz on recent phones) at the current
     * resolution. The game makes its own frame timing (30 or 60 fps); at 120 Hz a finished frame
     * waits at most 8.3 ms for the screen instead of 16.7, so a late frame shows up sooner and the
     * hitch is shorter. Android may still lower it (battery saver, heat). Off with
     * "lr_refresh = 60" in android_options.txt.
     */
    private void requestHighRefreshRate() {
        try {
            String user = getIntent() != null ? getIntent().getStringExtra(EXTRA_USER_ROOT) : null;
            if (user == null) user = LrPaths.userRoot(this);
            java.io.File opts = new java.io.File(user, "android_options.txt");
            if (opts.isFile()) {
                for (String line : java.nio.file.Files.readAllLines(opts.toPath())) {
                    String l = line.trim().replace(" ", "");
                    if (l.equals("lr_refresh=60")) return;
                }
            }
            android.view.Display display = getWindowManager().getDefaultDisplay();
            android.view.Display.Mode current = display.getMode();
            android.view.Display.Mode best = current;
            for (android.view.Display.Mode m : display.getSupportedModes()) {
                if (m.getPhysicalWidth() == current.getPhysicalWidth()
                        && m.getPhysicalHeight() == current.getPhysicalHeight()
                        && m.getRefreshRate() > best.getRefreshRate() + 0.5f) {
                    best = m;
                }
            }
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.preferredDisplayModeId = best.getModeId();
            getWindow().setAttributes(lp);
            Log.i(TAG, "display mode: " + best.getPhysicalWidth() + "x" + best.getPhysicalHeight()
                    + " at " + best.getRefreshRate() + " Hz (was " + current.getRefreshRate() + " Hz)");
        } catch (Exception e) {
            Log.w(TAG, "could not set the refresh rate", e);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (LrTouch.get() != null) LrTouch.get().onResume();
    }

    @Override
    protected void onPause() {
        if (LrTouch.get() != null) LrTouch.get().onPause();
        super.onPause();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (LrTouch.get() != null && LrTouch.get().onActivityResult(requestCode, resultCode, data)) return;
        super.onActivityResult(requestCode, resultCode, data);
    }

    @Override
    protected String[] getLibraries() {
        // librexruntime.so holds SDL3 (its JNI_OnLoad registers SDL's natives); libmain.so last:
        // SDL calls SDL_main from the last library listed.
        return new String[] {"c++_shared", "rexruntime", "main"};
    }

    @Override
    protected String[] getArguments() {
        Intent intent = getIntent();
        String game = intent != null ? intent.getStringExtra(EXTRA_GAME_ROOT) : null;
        String user = intent != null ? intent.getStringExtra(EXTRA_USER_ROOT) : null;
        if (game == null) game = LrPaths.gameRoot(this);
        if (user == null) user = LrPaths.userRoot(this);
        int generation = intent != null ? intent.getIntExtra(EXTRA_GENERATION, 0) : 0;
        new File(user, "logs").mkdirs();
        String log = new File(new File(user, "logs"),
                generation > 0 ? "lightningreturns_relaunch" + generation + ".log" : "lightningreturns.log").getAbsolutePath();
        ArrayList<String> args = new ArrayList<>();
        args.add("--game_data_root=" + game);
        args.add("--user_data_root=" + user);
        args.add("--log_file=" + log);
        Log.i(TAG, "arguments: " + args);
        return args.toArray(new String[0]);
    }

    /**
     * Called from native code (LrAndroidRestartProcess) when the game restarts itself; the PC
     * build starts a new copy of its executable instead. RestartActivity (own process) waits for
     * this process to end, then starts the game again. Native code ends this process right after.
     */
    public boolean lrRestartApp(int generation) {
        try {
            Intent i = new Intent(this, RestartActivity.class);
            i.putExtra(RestartActivity.EXTRA_PID, Process.myPid());
            Intent cur = getIntent();
            i.putExtra(EXTRA_GAME_ROOT, cur != null ? cur.getStringExtra(EXTRA_GAME_ROOT) : null);
            i.putExtra(EXTRA_USER_ROOT, cur != null ? cur.getStringExtra(EXTRA_USER_ROOT) : null);
            i.putExtra(EXTRA_GENERATION, generation);
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(i);
            return true;
        } catch (Exception e) {
            Log.e(TAG, "restart failed", e);
            return false;
        }
    }
}
