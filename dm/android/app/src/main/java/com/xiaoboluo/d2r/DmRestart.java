package com.xiaoboluo.d2r;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import java.io.File;

// [DOOM] Restart of the app in a new process (BUGS_AND_FIXES 2.28): a game folder chosen after nativeStart already ran (and failed)
// needs a fresh process, because the native side keeps its first DM_ROMFS / runtime / log paths. This activity runs in its own process
// (":restart", AndroidManifest.xml), ends the game's process (pid in the intent), waits until it is gone, starts DmActivity in a new task
// and ends itself. Only used while the game is not running (a start error screen), so no save can be cut off.
public class DmRestart extends Activity {
    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        int pid = getIntent().getIntExtra("pid", 0);
        if (pid > 0 && pid != android.os.Process.myPid()) {
            android.os.Process.killProcess(pid);
            for (int i = 0; i < 100 && new File("/proc/" + pid).exists(); i++) android.os.SystemClock.sleep(20);
        }
        startActivity(new Intent(this, DmActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK));
        finish();
        Runtime.getRuntime().exit(0);
    }
}
