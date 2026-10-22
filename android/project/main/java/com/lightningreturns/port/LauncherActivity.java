package com.lightningreturns.port;

import android.app.Activity;
import android.app.PendingIntent;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;

/**
 * First screen: all-files access (needed to read the game folder on shared storage), the game
 * folder and the user data folder, then Start (LrActivity). Plain views, no extra libraries.
 */
public class LauncherActivity extends Activity {
    private TextView status;
    private EditText gameRoot;
    private EditText userRoot;
    private Button grant;
    private Button start;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        int pad = (int) (16 * getResources().getDisplayMetrics().density);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("Lightning Returns");
        title.setTextSize(24);
        title.setTextColor(Color.WHITE);
        root.addView(title);

        status = new TextView(this);
        status.setTextSize(15);
        status.setPadding(0, pad / 2, 0, pad / 2);
        root.addView(status);

        root.addView(label("Game folder (the 360 game files: default.xex and its folders)"));
        gameRoot = new EditText(this);
        gameRoot.setSingleLine(true);
        gameRoot.setText(LrPaths.gameRoot(this));
        root.addView(gameRoot);

        root.addView(label("User data folder (saves, settings, logs)"));
        userRoot = new EditText(this);
        userRoot.setSingleLine(true);
        userRoot.setText(LrPaths.userRoot(this));
        root.addView(userRoot);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.START);
        grant = new Button(this);
        grant.setText("Allow file access");
        grant.setOnClickListener(v -> requestAllFiles());
        buttons.addView(grant);
        start = new Button(this);
        start.setText("Start");
        start.setOnClickListener(v -> startGame());
        buttons.addView(start);
        root.addView(buttons);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(scroll);
    }

    private TextView label(String text) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(13);
        return t;
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
        requestControllerAccess();
    }

    /**
     * Asks for access to USB controllers here, before the game starts (SDL would otherwise ask
     * once the game is running). Android keeps the permission for the app while the controller
     * stays plugged in. Bluetooth controllers need no permission.
     */
    private void requestControllerAccess() {
        UsbManager usb = (UsbManager) getSystemService(USB_SERVICE);
        if (usb == null) return;
        for (UsbDevice d : usb.getDeviceList().values()) {
            if (usb.hasPermission(d) || !isController(d)) continue;
            Intent intent = new Intent(getPackageName() + ".USB_PERMISSION");
            intent.setPackage(getPackageName());
            int flags = Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0;
            try {
                usb.requestPermission(d, PendingIntent.getBroadcast(this, 0, intent, flags));
            } catch (Exception ignored) {
            }
            return; // one dialog at a time; the next one comes when this screen resumes
        }
    }

    /** HID devices and Xbox controllers (vendor-specific class 255, subclass 71 or 93). */
    private static boolean isController(UsbDevice d) {
        for (int i = 0; i < d.getInterfaceCount(); ++i) {
            UsbInterface f = d.getInterface(i);
            if (f.getInterfaceClass() == UsbConstants.USB_CLASS_HID) return true;
            if (f.getInterfaceClass() == UsbConstants.USB_CLASS_VENDOR_SPEC
                    && (f.getInterfaceSubclass() == 71 || f.getInterfaceSubclass() == 93)) return true;
        }
        return false;
    }

    private boolean hasAllFiles() {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager();
    }

    private void refresh() {
        boolean access = hasAllFiles();
        String game = gameRoot.getText().toString().trim();
        StringBuilder sb = new StringBuilder();
        if (!access) {
            sb.append("File access is not allowed yet: tap \"Allow file access\" and enable it for this app.\n");
        } else if (!new File(game).isDirectory()) {
            sb.append("The game folder does not exist yet. Copy the game files to:\n").append(game).append('\n');
        } else if (!LrPaths.looksLikeGameRoot(game)) {
            sb.append("No default.xex in the game folder - is this the right folder?\n");
        } else {
            sb.append("Ready.\n");
        }
        sb.append("Resolution: 720p or 1440p, in the game's Settings > Resolution.");
        status.setText(sb.toString());
        grant.setEnabled(!access);
        start.setEnabled(access);
    }

    private void requestAllFiles() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName())));
            } catch (Exception e) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        } else {
            requestPermissions(new String[] {
                    android.Manifest.permission.READ_EXTERNAL_STORAGE,
                    android.Manifest.permission.WRITE_EXTERNAL_STORAGE}, 1);
        }
    }

    private void startGame() {
        String game = gameRoot.getText().toString().trim();
        String user = userRoot.getText().toString().trim();
        if (user.isEmpty()) user = LrPaths.defaultUserRoot(game);
        LrPaths.save(this, game, user);
        new File(user).mkdirs();
        Intent i = new Intent(this, LrActivity.class);
        i.putExtra(LrActivity.EXTRA_GAME_ROOT, game);
        i.putExtra(LrActivity.EXTRA_USER_ROOT, user);
        startActivity(i);
        finish();
    }
}
