package io.anyport.anyswf;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Runs one compiled movie: the game and runtime are this APK's libmain.so (SDL linked in
 * statically) and the converted movie is the asset assets.bin, which the player opens
 * through SDL's asset fallback. Saves go to getFilesDir() (SDL's pref path).
 */
public class GameActivity extends SDLActivity {
    static final String TAG = "anyswf";
    /** Player arguments chosen at build time (manifest meta-data), space separated. */
    static final String META_ARGS = "io.anyport.anyswf.ARGS";
    /** Extra player arguments, e.g. adb shell am start ... --esa args --frames,600 */
    public static final String EXTRA_ARGS = "args";

    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    @Override
    protected String[] getArguments() {
        List<String> args = new ArrayList<>();
        try {
            ApplicationInfo info = getPackageManager().getApplicationInfo(getPackageName(), PackageManager.GET_META_DATA);
            Bundle meta = info.metaData;
            String built = meta == null ? null : meta.getString(META_ARGS);
            if (built != null) {
                for (String a : built.trim().split("\\s+")) {
                    if (!a.isEmpty()) args.add(a);
                }
            }
        } catch (PackageManager.NameNotFoundException e) {
            Log.w(TAG, "cannot read the build arguments", e);
        }
        String[] extra = getIntent() == null ? null : getIntent().getStringArrayExtra(EXTRA_ARGS);
        if (extra != null) args.addAll(Arrays.asList(extra));
        return args.toArray(new String[0]);
    }
}
