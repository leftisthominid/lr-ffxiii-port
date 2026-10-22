package com.lightningreturns.port;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;

import java.io.File;

/** Runs in the ":restart" process: waits for the old game process to exit, then starts the game again. */
public class RestartActivity extends Activity {
    static final String EXTRA_PID = "old_pid";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        final Intent from = getIntent();
        final int pid = from.getIntExtra(EXTRA_PID, -1);
        new Thread(() -> {
            for (int i = 0; i < 1500 && pid > 0 && new File("/proc/" + pid).exists(); ++i) {
                try { Thread.sleep(10); } catch (InterruptedException ignored) {}
            }
            runOnUiThread(() -> {
                Intent i = new Intent(this, LrActivity.class);
                i.putExtra(LrActivity.EXTRA_GAME_ROOT, from.getStringExtra(LrActivity.EXTRA_GAME_ROOT));
                i.putExtra(LrActivity.EXTRA_USER_ROOT, from.getStringExtra(LrActivity.EXTRA_USER_ROOT));
                i.putExtra(LrActivity.EXTRA_GENERATION, from.getIntExtra(LrActivity.EXTRA_GENERATION, 0));
                i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
                startActivity(i);
                finish();
                Process.killProcess(Process.myPid());
            });
        }).start();
    }
}
