package com.rayrod.goldenballoon;

import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;

import java.io.File;

import org.libsdl.app.SDLActivity;

/**
 * The game activity. Every piece of VR work happens in native code (OpenXR); this class guards
 * the one Android prerequisite the engine cannot recover from mid-boot -- getting at the
 * player's ROM. Ask for access, then STOP: booting the engine without a ROM would fail on its
 * data before the player ever saw the permission toggle.
 */
public class GoldenBalloonActivity extends SDLActivity {

    // The names a Diddy Kong Racing dump arrives under. The first is what the hub's Send to
    // Quest writes; the rest are what a hand-dropped dump is usually called.
    private static final String[] ROM_NAMES = {
        "dkr.us.v80.z64",
        "baserom.us.v80.z64",
        "dkr.z64",
        "Diddy Kong Racing (U) (V1.1).z64",
    };

    private boolean hasAllFilesAccess() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
            return true;
        }
        return Environment.isExternalStorageManager();
    }

    private void requestAllFilesAccess() {
        try {
            Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
            startActivity(intent);
        } catch (Exception e) {
            // Some builds lack the per-app screen - fall back to the full list.
            Intent intent = new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION);
            startActivity(intent);
        }
    }

    // Send to Quest pushes the ROM straight into this app's own files dir, and the engine reads
    // app-local files with no permission at all - the all-files scan is only the fallback for a
    // dump the player dropped somewhere else. So when the ROM is already in place, boot. On the
    // SM64 build the permission screen at first launch read as "the game will not start" to
    // everyone who installed through the hub.
    private boolean hasLocalRom() {
        File dir = getExternalFilesDir(null);
        if (dir == null) {
            return false;
        }
        for (String name : ROM_NAMES) {
            if (new File(dir, name).exists()) {
                return true;
            }
        }
        return false;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        if (!hasAllFilesAccess() && !hasLocalRom()) {
            requestAllFilesAccess();
            super.onCreate(savedInstanceState);
            finish(); // ask, then stop - relaunch boots the game with access in hand
            return;
        }
        super.onCreate(savedInstanceState);
    }

    // onResume deliberately does NOT re-ask: doing so opened the settings screen twice in a row
    // on the SM64 build. One ask per launch.

    // libopenxr_loader.so is a DT_NEEDED of libmain.so, so the linker pulls it out of the APK on
    // its own; naming it here as well would load it twice.
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }
}
