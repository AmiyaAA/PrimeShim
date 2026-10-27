package dev.primeshim.android;

import android.app.Application;
import androidx.appcompat.app.AppCompatDelegate;
import com.google.android.material.color.DynamicColors;

/** Material You dynamic colour for every activity (Android 12+). */
public final class PrimeShimApp extends Application {
    @Override
    public void onCreate() {
        super.onCreate();
        int theme = getSharedPreferences("primeshim", MODE_PRIVATE)
                .getInt("theme", AppCompatDelegate.MODE_NIGHT_FOLLOW_SYSTEM);
        AppCompatDelegate.setDefaultNightMode(theme);
        DynamicColors.applyToActivitiesIfAvailable(this);
    }
}
